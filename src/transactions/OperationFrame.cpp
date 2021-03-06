// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "OperationFrame.h"
#include "main/Application.h"
#include "xdrpp/marshal.h"
#include <string>
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "transactions/TransactionFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/CreatePassiveOfferOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"
#include "transactions/MergeOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/PaymentExternalOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/ManageDataOpFrame.h"
#include "transactions/AdministrativeOpFrame.h"
#include "transactions/PaymentReversalOpFrame.h"
#include "database/Database.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;

shared_ptr<OperationFrame>
OperationFrame::makeHelper(Operation const& op, OperationResult& res, OperationFee* fee,
                           TransactionFrame& tx)
{
    switch (op.body.type())
    {
    case CREATE_ACCOUNT:
        return shared_ptr<OperationFrame>(new CreateAccountOpFrame(op, res, fee, tx));
    case PAYMENT:
        return shared_ptr<OperationFrame>(new PaymentOpFrame(op, res, fee, tx));
    case PATH_PAYMENT:
        return shared_ptr<OperationFrame>(new PathPaymentOpFrame(op, res, fee, tx));
    case MANAGE_OFFER:
        return shared_ptr<OperationFrame>(new ManageOfferOpFrame(op, res, fee, tx));
    case CREATE_PASSIVE_OFFER:
        return shared_ptr<OperationFrame>(new CreatePassiveOfferOpFrame(op, res, fee, tx));
    case SET_OPTIONS:
        return shared_ptr<OperationFrame>(new SetOptionsOpFrame(op, res, fee, tx));
    case CHANGE_TRUST:
        return shared_ptr<OperationFrame>(new ChangeTrustOpFrame(op, res, fee, tx));
    case ALLOW_TRUST:
        return shared_ptr<OperationFrame>(new AllowTrustOpFrame(op, res, fee, tx));
    case ACCOUNT_MERGE:
        return shared_ptr<OperationFrame>(new MergeOpFrame(op, res, fee, tx));
    case INFLATION:
        return shared_ptr<OperationFrame>(new InflationOpFrame(op, res, fee, tx));
    case MANAGE_DATA:
        return shared_ptr<OperationFrame>(new ManageDataOpFrame(op, res, fee, tx));
	case ADMINISTRATIVE:
		return shared_ptr<OperationFrame>(new AdministrativeOpFrame(op, res, fee, tx));
	case PAYMENT_REVERSAL:
		return shared_ptr<OperationFrame>(new PaymentReversalOpFrame(op, res, fee, tx));
    case EXTERNAL_PAYMENT:
        return shared_ptr<OperationFrame>(new PaymentExternalOpFrame(op, res, fee, tx));

    default:
        CLOG(DEBUG, "Process") << "operation " << op.body.type() << " is unknown ";
        ostringstream err;
        err << "Unknown Tx type: " << op.body.type();
        throw std::invalid_argument(err.str());
    }
}

OperationFrame::OperationFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                               TransactionFrame& parentTx)
    : mOperation(op), mParentTx(parentTx), mResult(res), mFee(fee)
{
}

bool
OperationFrame::apply(LedgerDelta& delta, Application& app)
{
    bool res;
    res = checkValid(app, &delta);
    if (res)
    {
        res = doApply(app, delta, app.getLedgerManager());
    }

    return res;
}

int32_t
OperationFrame::getNeededThreshold() const
{
	return mSourceAccount->getMediumThreshold();
}

bool
OperationFrame::checkSignature()
{
    return mParentTx.checkSignature(*mSourceAccount, getNeededThreshold(), &mUsedSigners);
}

AccountID const&
OperationFrame::getSourceID() const
{
    return mOperation.sourceAccount ? *mOperation.sourceAccount
                                    : mParentTx.getEnvelope().tx.sourceAccount;
}

bool
OperationFrame::loadAccount(LedgerDelta* delta, Database& db)
{
    mSourceAccount = mParentTx.loadAccount(delta, db, getSourceID());
    return !!mSourceAccount;
}

OperationResultCode
OperationFrame::getResultCode() const
{
    return mResult.code();
}

// called when determining if we should accept this operation.
// called when determining if we should flood
// make sure sig is correct
// verifies that the operation is well formed (operation specific)
bool
OperationFrame::checkValid(Application& app, LedgerDelta* delta)
{

    bool forApply = (delta != nullptr);
    if (!loadAccount(delta, app.getDatabase()))
    {
        if (forApply || !mOperation.sourceAccount)
        {
            app.getMetrics()
                .NewMeter({"operation", "invalid", "no-account"}, "operation")
                .Mark();
            mResult.code(opNO_ACCOUNT);
            return false;
        }
        else
        {
            mSourceAccount =
                AccountFrame::makeAuthOnlyAccount(*mOperation.sourceAccount);
        }
    }

    if (!checkSignature())
    {
        app.getMetrics()
            .NewMeter({"operation", "invalid", "bad-auth"}, "operation")
            .Mark();
        mResult.code(opBAD_AUTH);
        return false;
    }

    if (!forApply)
    {
        // safety: operations should not rely on ledger state as
        // previous operations may change it (can even create the account)
        mSourceAccount.reset();
    }

    mResult.code(opINNER);
    mResult.tr().type(mOperation.body.type());

    return doCheckValid(app);
}

TrustFrame::pointer
OperationFrame::createTrustLine(Application& app, LedgerManager& ledgerManager, LedgerDelta& delta, TransactionFrame& parentTx, AccountFrame::pointer account, Asset const& asset)
{
	// build a changeTrustOp
	Operation op;
	op.sourceAccount.activate() = account->getID();
	op.body.type(CHANGE_TRUST);
	ChangeTrustOp& caOp = op.body.changeTrustOp();
	caOp.limit = INT64_MAX;
	caOp.line = asset;

	OperationResult opRes;
	opRes.code(opINNER);
	opRes.tr().type(CHANGE_TRUST);

	//no need to take fee twice
	OperationFee fee;
	fee.type(OperationFeeType::opFEE_NONE);

	ChangeTrustOpFrame changeTrust(op, opRes, &fee, parentTx);
	changeTrust.setSourceAccountPtr(account);

	// create trust line
	if (!changeTrust.doCheckValid(app) ||
		!changeTrust.doApply(app, delta, ledgerManager))
	{
		if (changeTrust.getResultCode() != opINNER)
		{
			throw std::runtime_error("Unexpected error code from changeTrust");
		}
		switch (ChangeTrustOpFrame::getInnerCode(changeTrust.getResult()))
		{
		case CHANGE_TRUST_NO_ISSUER:
		case CHANGE_TRUST_LOW_RESERVE:
			return nullptr;
		case CHANGE_TRUST_MALFORMED:
			app.getMetrics().NewMeter({ "op", "failure", "malformed-change-trust-op" }, "operation").Mark();
			throw std::runtime_error("Failed to create trust line - change trust line op is malformed");
		case CHANGE_TRUST_INVALID_LIMIT:
			app.getMetrics().NewMeter({ "op", "failure", "invalid-limit-change-trust-op" }, "operation").Mark();
			throw std::runtime_error("Failed to create trust line - invalid limit");
		default:
			throw std::runtime_error("Unexpected error code from change trust line");
		}
	}
	return changeTrust.getTrustLine();
}


}
