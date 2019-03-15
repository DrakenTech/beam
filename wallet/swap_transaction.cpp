//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "swap_transaction.h"

#include "bitcoin/bitcoin.hpp"
#include "nlohmann/json.hpp"

using namespace std;
using namespace ECC;
using json = nlohmann::json;

namespace beam::wallet
{
    namespace
    {
        uint32_t kBeamLockTimeInBlocks = 24 * 60;
        uint32_t kBTCLockTimeSec = 2 * 24 * 60 * 60;
        uint32_t kBTCMinTxConfirmations = 6;

        void InitSecret(BaseTransaction& transaction, SubTxID subTxID)
        {
            NoLeak<uintBig> preimage;
            GenRandom(preimage.V);
            transaction.SetParameter(TxParameterID::PreImage, preimage.V, false, subTxID);
        }

        libbitcoin::chain::script AtomicSwapContract(const libbitcoin::short_hash& hashPublicKeyA
                                                   , const libbitcoin::short_hash& hashPublicKeyB
                                                   , int64_t locktime
                                                   , const libbitcoin::data_chunk& secretHash
                                                   , size_t secretSize)
        {
            using namespace libbitcoin::machine;

            operation::list contract_operations;

            contract_operations.emplace_back(operation(opcode::if_)); // Normal redeem path
            {
                // Require initiator's secret to be a known length that the redeeming
                // party can audit.  This is used to prevent fraud attacks between two
                // currencies that have different maximum data sizes.
                contract_operations.emplace_back(operation(opcode::size));
                operation secretSizeOp;
                secretSizeOp.from_string(std::to_string(secretSize));
                contract_operations.emplace_back(secretSizeOp);
                contract_operations.emplace_back(operation(opcode::equalverify));

                // Require initiator's secret to be known to redeem the output.
                contract_operations.emplace_back(operation(opcode::sha256));
                contract_operations.emplace_back(operation(secretHash));
                contract_operations.emplace_back(operation(opcode::equalverify));

                // Verify their signature is being used to redeem the output.  This
                // would normally end with OP_EQUALVERIFY OP_CHECKSIG but this has been
                // moved outside of the branch to save a couple bytes.
                contract_operations.emplace_back(operation(opcode::dup));
                contract_operations.emplace_back(operation(opcode::hash160));
                contract_operations.emplace_back(operation(libbitcoin::to_chunk(hashPublicKeyB)));
            }
            contract_operations.emplace_back(operation(opcode::else_)); // Refund path
            {
                // Verify locktime and drop it off the stack (which is not done by CLTV).
                operation locktimeOp;
                locktimeOp.from_string(std::to_string(locktime));
                contract_operations.emplace_back(locktimeOp);
                contract_operations.emplace_back(operation(opcode::checklocktimeverify));
                contract_operations.emplace_back(operation(opcode::drop));

                // Verify our signature is being used to redeem the output.  This would
                // normally end with OP_EQUALVERIFY OP_CHECKSIG but this has been moved
                // outside of the branch to save a couple bytes.
                contract_operations.emplace_back(operation(opcode::dup));
                contract_operations.emplace_back(operation(opcode::hash160));
                contract_operations.emplace_back(operation(libbitcoin::to_chunk(hashPublicKeyA)));
            }
            contract_operations.emplace_back(operation(opcode::endif));

            // Complete the signature check.
            contract_operations.emplace_back(operation(opcode::equalverify));
            contract_operations.emplace_back(operation(opcode::checksig));

            return libbitcoin::chain::script(contract_operations);
        }

        libbitcoin::chain::script CreateAtomicSwapContract(const BaseTransaction& transaction)
        {
            // TODO: change locktime
            Timestamp locktime = transaction.GetMandatoryParameter<Timestamp>(TxParameterID::CreateTime) + kBTCLockTimeSec;
            std::string peerSwapAddress = transaction.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapPeerAddress);
            std::string swapAddress = transaction.GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);

            // load secret or secretHash
            uintBig preimage = transaction.GetMandatoryParameter<uintBig>(TxParameterID::PreImage, AtomicSwapTransaction::SubTxIndex::BEAM_REDEEM_TX);
            Hash::Value lockImage(Zero);
            Hash::Processor() << preimage >> lockImage;

            libbitcoin::data_chunk secretHash = libbitcoin::to_chunk(lockImage.m_pData);
            // TODO: sender || receiver
            libbitcoin::wallet::payment_address senderAddress(swapAddress);
            libbitcoin::wallet::payment_address receiverAddress(peerSwapAddress);

            return AtomicSwapContract(senderAddress.hash(), receiverAddress.hash(), locktime, secretHash, secretHash.size());
        }
    }

    AtomicSwapTransaction::AtomicSwapTransaction(INegotiatorGateway& gateway
                                               , beam::IWalletDB::Ptr walletDB
                                               , const TxID& txID)
        : BaseTransaction(gateway, walletDB, txID)
    {

    }

    bool AtomicSwapTransaction::SetRegisteredStatus(Transaction::Ptr transaction, bool isRegistered)
    {
        Merkle::Hash kernelID;
        transaction->m_vKernels.back()->get_ID(kernelID);

        SubTxIndex subTxID = SubTxIndex::BEAM_LOCK_TX;
        Merkle::Hash lockTxKernelID = GetMandatoryParameter<Merkle::Hash>(TxParameterID::KernelID, SubTxIndex::BEAM_LOCK_TX);

        if (kernelID != lockTxKernelID)
        {
            subTxID = IsSender() ? SubTxIndex::BEAM_REFUND_TX : SubTxIndex::BEAM_REDEEM_TX;
        }

        return SetParameter(TxParameterID::TransactionRegistered, isRegistered, false, subTxID);
    }

    void AtomicSwapTransaction::SetNextState(State state)
    {
        SetState(state);
        UpdateAsync();
    }

    void AtomicSwapTransaction::UpdateAsync()
    {
        if (!m_EventToUpdate)
        {
            m_EventToUpdate = io::AsyncEvent::create(io::Reactor::get_Current(), [this]() { UpdateImpl(); });
        }

        m_EventToUpdate->post();
    }

    TxType AtomicSwapTransaction::GetType() const
    {
        return TxType::AtomicSwap;
    }

    AtomicSwapTransaction::State AtomicSwapTransaction::GetState(SubTxID subTxID) const
    {
        State state = State::Initial;
        GetParameter(TxParameterID::State, state, subTxID);
        return state;
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::GetSubTxState(SubTxID subTxID) const
    {
        SubTxState state = SubTxState::Initial;
        GetParameter(TxParameterID::State, state, subTxID);
        return state;
    }

    void AtomicSwapTransaction::UpdateImpl()
    {
        State state = GetState(kDefaultSubTxID);
        bool isBeamOwner = IsSender();

        switch (state)
        {
        case State::Initial:
        {
            // load or generate BTC address
            std::string swapAddress;

            if (!GetParameter(TxParameterID::AtomicSwapAddress, swapAddress))
            {
                // is need to setup type 'legacy'?
                m_Gateway.get_bitcoin_rpc()->getRawChangeAddress(BIND_THIS_MEMFN(OnGetRawChangeAddress));
                break;
            }

            SetNextState(State::Invitation);
            break;
        }
        case State::Invitation:
        {
            if (IsInitiator())
            {
                SendInvitation();
            }
            
            SetNextState(isBeamOwner ? State::BuildingBeamLockTX : State::BuildingLockTX);
            break;
        }
        case State::BuildingLockTX:
        {
            assert(!isBeamOwner);
            auto lockTxState = BuildLockTx();
            if (lockTxState != SwapTxState::Constructed)
                break;

            SetNextState(State::BuildingBeamLockTX);
            break;
        }
        case State::BuildingRefundTX:
        {
            assert(!isBeamOwner);
            auto refundTxState = BuildWithdrawTx(SubTxIndex::REFUND_TX);
            if (refundTxState != SwapTxState::Constructed)
                break;

            SetNextState(State::SendingBeamLockTX);
            break;
        }
        case State::BuildingRedeemTX:
        {
            assert(isBeamOwner);
            auto refundTxState = BuildWithdrawTx(SubTxIndex::REDEEM_TX);
            if (refundTxState != SwapTxState::Constructed)
                break;

            SetNextState(State::SendingRedeemTX);
            break;
        }
        case State::BuildingBeamLockTX:
        {
            auto lockTxState = BuildBeamLockTx();
            if (lockTxState != SubTxState::Constructed)
                break;

            SetNextState(State::BuildingBeamRefundTX);
            break;
        }
        case State::BuildingBeamRefundTX:
        {
            auto subTxState = BuildBeamRefundTx();
            if (subTxState != SubTxState::Constructed)
                break;

            SetNextState(State::BuildingBeamRedeemTX);
            break;
        }
        case State::BuildingBeamRedeemTX:
        {
            auto subTxState = BuildBeamRedeemTx();
            if (subTxState != SubTxState::Constructed)
                break;

            SetNextState(State::HandlingContractTX);
            break;
        }
        case State::HandlingContractTX:
        {
            if (!isBeamOwner)
            {
                // send contractTx
                assert(m_SwapLockRawTx.is_initialized());

                if (!RegisterExternalTx(*m_SwapLockRawTx, SubTxIndex::LOCK_TX))
                    break;

                SendExternalTxDetails();
                SetNextState(State::BuildingRefundTX);
            }
            else
            {
                // wait TxID from peer
                std::string txID;
                if (!GetParameter(TxParameterID::AtomicSwapExternalTxID, txID, SubTxIndex::LOCK_TX))
                    break;

                // TODO: check current blockchain height and cancel swap if too late

                if (m_SwapLockTxConfirmations < kBTCMinTxConfirmations)
                {
                    // validate expired?

                    // TODO: timeout ?
                    GetSwapLockTxConfirmations();
                    break;
                }
                SetNextState(State::SendingBeamLockTX);
            }
            break;
        }
        case State::SendingRefundTX:
        {
            assert(false && "Not implemented yet.");
            break;
        }
        case State::SendingRedeemTX:
        {
            assert(false && "Not implemented yet.");
            break;
        }
        case State::SendingBeamLockTX:
        {
            // TODO: load m_LockTx
            if (m_LockTx && !SendSubTx(m_LockTx, SubTxIndex::BEAM_LOCK_TX))
                break;

            if (!isBeamOwner)
            {
                // validate second chain height (second coin timelock)
                // SetNextState(State::SendingRefundTX);
            }

            if (!IsSubTxCompleted(SubTxIndex::BEAM_LOCK_TX))
                break;
            
            LOG_DEBUG() << GetTxID()<< " Lock TX completed.";

            // TODO: change this (dirty hack)
            SetParameter(TxParameterID::KernelProofHeight, Height(0));

            SetNextState(State::SendingBeamRedeemTX);
            break;
        }
        case State::SendingBeamRedeemTX:
        {
            if (m_RedeemTx && !SendSubTx(m_RedeemTx, SubTxIndex::BEAM_REDEEM_TX))
                break;

            if (isBeamOwner)
            {
                if (IsBeamLockTimeExpired())
                {
                    LOG_DEBUG() << GetTxID() << " Beam locktime expired.";

                    SetNextState(State::SendingBeamRefundTX);
                    break;
                }

                // request kernel body for getting secret(preimage)
                ECC::uintBig preimage(Zero);
                if (!GetPreimageFromChain(preimage))
                    break;

                LOG_DEBUG() << GetTxID() << " Got preimage: " << preimage;
                
                // Redeem second Coin
                SetNextState(State::BuildingRedeemTX);
            }
            else
            {
                if (!IsSubTxCompleted(SubTxIndex::BEAM_REDEEM_TX))
                    break;

                LOG_DEBUG() << GetTxID() << " Redeem TX completed!";

                SetNextState(State::CompleteSwap);
            }
            break;
        }
        case State::SendingBeamRefundTX:
        {
            assert(isBeamOwner);

            if (m_RefundTx && !SendSubTx(m_RefundTx, SubTxIndex::BEAM_REFUND_TX))
                break;

            if (!IsSubTxCompleted(SubTxIndex::BEAM_REFUND_TX))
                break;

            LOG_DEBUG() << GetTxID() << " Refund TX completed!";

            SetNextState(State::CompleteSwap);
            break;
        }
        case State::CompleteSwap:
        {
            LOG_DEBUG() << GetTxID() << " Swap completed.";

            UpdateTxDescription(TxStatus::Completed);
            break;
        }

        default:
            break;
        }
    }

    AtomicSwapTransaction::SwapTxState AtomicSwapTransaction::BuildLockTx()
    {
        SwapTxState swapTxState = SwapTxState::Initial;
        GetParameter(TxParameterID::State, swapTxState, SubTxIndex::LOCK_TX);
        
        if (swapTxState == SwapTxState::Initial)
        {
            InitSecret(*this, SubTxIndex::BEAM_REDEEM_TX);

            auto contractScript = CreateAtomicSwapContract(*this);

            Amount swapAmount = GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);
            libbitcoin::chain::transaction contractTx;
            libbitcoin::chain::output output(swapAmount, contractScript);
            contractTx.outputs().push_back(output);

            std::string hexTx = libbitcoin::encode_base16(contractTx.to_data());

            m_Gateway.get_bitcoin_rpc()->fundRawTransaction(hexTx, BIND_THIS_MEMFN(OnFundRawTransaction));
            SetState(SwapTxState::CreatingTx, SubTxIndex::LOCK_TX);
            return SwapTxState::CreatingTx;
        }

        if (swapTxState == SwapTxState::CreatingTx)
        {
            // TODO: implement
        }

        // TODO: check
        return swapTxState;
    }

    AtomicSwapTransaction::SwapTxState AtomicSwapTransaction::BuildWithdrawTx(SubTxID subTxID)
    {
        SwapTxState swapTxState = SwapTxState::Initial;
        GetParameter(TxParameterID::State, swapTxState, subTxID);

        if (swapTxState == SwapTxState::Initial)
        {
            Amount swapAmount = GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);
            std::string swapAddress = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);
            int outputIndex = GetMandatoryParameter<int>(TxParameterID::AtomicSwapExternalTxOutputIndex, SubTxIndex::LOCK_TX);
            auto swapLockTxID = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTxID, SubTxIndex::LOCK_TX);

            std::vector<std::string> args;
            args.emplace_back("[{\"txid\": \"" + swapLockTxID + "\", \"vout\":" + std::to_string(outputIndex) + ", \"Sequence\": " + std::to_string(libbitcoin::max_input_sequence - 1) + " }]");
            args.emplace_back("[{\"" + swapAddress + "\": " + std::to_string(double(swapAmount) / libbitcoin::satoshi_per_bitcoin) + "}]");
            if (subTxID == SubTxIndex::REFUND_TX)
            {
                Timestamp locktime = GetMandatoryParameter<Timestamp>(TxParameterID::CreateTime) + kBTCLockTimeSec;
                args.emplace_back(std::to_string(locktime));
            }

            m_Gateway.get_bitcoin_rpc()->createRawTransaction(args, BIND_THIS_MEMFN(OnCreateRefundTransaction));

            SetState(SwapTxState::CreatingTx, subTxID);
            return SwapTxState::CreatingTx;
        }

        if (swapTxState == SwapTxState::CreatingTx)
        {
            std::string swapAddress = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);
            auto callback = (subTxID == SubTxIndex::REFUND_TX) ? BIND_THIS_MEMFN(OnDumpSenderPrivateKey) : BIND_THIS_MEMFN(OnDumpReceiverPrivateKey);
            m_Gateway.get_bitcoin_rpc()->dumpPrivKey(swapAddress, callback);
        }

        return swapTxState;
    }

    bool AtomicSwapTransaction::RegisterExternalTx(const std::string& rawTransaction, SubTxID subTxID)
    {
        bool isRegistered = false;
        if (!GetParameter(TxParameterID::TransactionRegistered, isRegistered, subTxID))
        {
            auto callback = [this, subTxID](const std::string& response) {
                assert(!response.empty());

                json reply = json::parse(response);
                assert(reply["error"].empty());

                auto txID = reply["result"].get<std::string>();
                bool isRegistered = !txID.empty();
                SetParameter(TxParameterID::TransactionRegistered, isRegistered, false, subTxID);

                if (!txID.empty())
                {
                    SetParameter(TxParameterID::AtomicSwapExternalTxID, txID, false, subTxID);
                }

                Update();
            };

            m_Gateway.get_bitcoin_rpc()->sendRawTransaction(rawTransaction, callback);
            return isRegistered;
        }

        if (!isRegistered)
        {
            OnFailed(TxFailureReason::FailedToRegister, true);
        }

        return isRegistered;
    }

    void AtomicSwapTransaction::GetSwapLockTxConfirmations()
    {
        auto txID = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTxID, SubTxIndex::LOCK_TX);
        int outputIndex = GetMandatoryParameter<int>(TxParameterID::AtomicSwapExternalTxOutputIndex, SubTxIndex::LOCK_TX);

        m_Gateway.get_bitcoin_rpc()->getTxOut(txID, outputIndex, BIND_THIS_MEMFN(OnGetSwapLockTxConfirmations));
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::BuildBeamLockTx()
    {
        // load state
        SubTxState lockTxState = SubTxState::Initial;
        GetParameter(TxParameterID::State, lockTxState, SubTxIndex::BEAM_LOCK_TX);

        bool isSender = IsSender();
        auto lockTxBuilder = std::make_unique<LockTxBuilder>(*this, GetAmount(), GetMandatoryParameter<Amount>(TxParameterID::Fee));

        if (!lockTxBuilder->GetInitialTxParams() && lockTxState == SubTxState::Initial)
        {
            // TODO: check expired!

            if (isSender)
            {
                lockTxBuilder->SelectInputs();
                lockTxBuilder->AddChangeOutput();
            }

            if (!lockTxBuilder->FinalizeOutputs())
            {
                // TODO: transaction is too big :(
            }

            UpdateTxDescription(TxStatus::InProgress);
        }

        lockTxBuilder->CreateKernel();

        if (!lockTxBuilder->GetPeerPublicExcessAndNonce())
        {
            if (lockTxState == SubTxState::Initial && IsInitiator())
            {
                SendLockTxInvitation(*lockTxBuilder, isSender);
                SetState(SubTxState::Invitation, SubTxIndex::BEAM_LOCK_TX);
                lockTxState = SubTxState::Invitation;
            }
            return lockTxState;
        }

        lockTxBuilder->LoadSharedParameters();
        lockTxBuilder->SignPartial();

        if (lockTxState == SubTxState::Initial || lockTxState == SubTxState::Invitation)
        {
            lockTxBuilder->SharedUTXOProofPart2(isSender);
            SendBulletProofPart2(*lockTxBuilder, isSender);
            SetState(SubTxState::SharedUTXOProofPart2, SubTxIndex::BEAM_LOCK_TX);
            lockTxState = SubTxState::SharedUTXOProofPart2;
            return lockTxState;
        }

        assert(lockTxBuilder->GetPeerSignature());
        if (!lockTxBuilder->IsPeerSignatureValid())
        {
            LOG_INFO() << GetTxID() << " Peer signature is invalid.";
            return lockTxState;
        }

        lockTxBuilder->FinalizeSignature();

        if (lockTxState == SubTxState::SharedUTXOProofPart2)
        {
            lockTxBuilder->SharedUTXOProofPart3(isSender);
            SendBulletProofPart3(*lockTxBuilder, isSender);
            SetState(SubTxState::Constructed, SubTxIndex::BEAM_LOCK_TX);
            lockTxState = SubTxState::Constructed;
        }

        if (isSender && lockTxState == SubTxState::Constructed)
        {
            // Create TX
            auto transaction = lockTxBuilder->CreateTransaction();
            beam::TxBase::Context context;
            if (!transaction->IsValid(context))
            {
                // TODO: check
                OnFailed(TxFailureReason::InvalidTransaction, true);
                return lockTxState;
            }

            // TODO: return
            m_LockTx = transaction;

            return lockTxState;
        }

        return lockTxState;
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::BuildBeamRefundTx()
    {
        SubTxID subTxID = SubTxIndex::BEAM_REFUND_TX;
        SubTxState subTxState = GetSubTxState(subTxID);
        // TODO: calculating fee!
        Amount refundFee = 0;
        Amount refundAmount = GetAmount() - refundFee;
        bool isTxOwner = IsSender();
        SharedTxBuilder builder{ *this, subTxID, refundAmount, refundFee };

        if (!builder.GetSharedParameters())
        {
            return subTxState;
        }

        // send invite to get 
        if (!builder.GetInitialTxParams() && subTxState == SubTxState::Initial)
        {
            // TODO: check expired!
            builder.InitTx(isTxOwner);
        }

        builder.CreateKernel();

        if (!builder.GetPeerPublicExcessAndNonce())
        {
            if (subTxState == SubTxState::Initial && isTxOwner)
            {
                SendSharedTxInvitation(builder);
                SetState(SubTxState::Invitation, subTxID);
                subTxState = SubTxState::Invitation;
            }
            return subTxState;
        }

        builder.SignPartial();

        if (!builder.GetPeerSignature())
        {
            if (subTxState == SubTxState::Initial && !isTxOwner)
            {
                // invited participant
                assert(!IsInitiator());
                ConfirmSharedTxInvitation(builder);
                SetState(SubTxState::Constructed, subTxID);
                subTxState = SubTxState::Constructed;
            }
            return subTxState;
        }

        if (!builder.IsPeerSignatureValid())
        {
            LOG_INFO() << GetTxID() << " Peer signature is invalid.";
            return subTxState;
        }

        builder.FinalizeSignature();

        SetState(SubTxState::Constructed, subTxID);
        subTxState = SubTxState::Constructed;

        if (isTxOwner)
        {
            auto transaction = builder.CreateTransaction();
            beam::TxBase::Context context;
            transaction->IsValid(context);

            m_RefundTx = transaction;
        }

        return subTxState;
    }

    AtomicSwapTransaction::SubTxState AtomicSwapTransaction::BuildBeamRedeemTx()
    {
        SubTxID subTxID = SubTxIndex::BEAM_REDEEM_TX;
        SubTxState subTxState = GetSubTxState(subTxID);
        // TODO: calculating fee!
        Amount redeemFee = 0;
        Amount redeemAmount = GetAmount() - redeemFee;
        bool isTxOwner = !IsSender();
        SharedTxBuilder builder{ *this, subTxID, redeemAmount, redeemFee };

        if (!builder.GetSharedParameters())
        {
            return subTxState;
        }

        // send invite to get 
        if (!builder.GetInitialTxParams() && subTxState == SubTxState::Initial)
        {
            // TODO: check expired!
            builder.InitTx(isTxOwner);
        }

        builder.CreateKernel();

        if (!builder.GetPeerPublicExcessAndNonce())
        {
            if (subTxState == SubTxState::Initial && isTxOwner)
            {
                // send invitation with LockImage
                SendSharedTxInvitation(builder, true);
                SetState(SubTxState::Invitation, subTxID);
                subTxState = SubTxState::Invitation;
            }
            return subTxState;
        }

        builder.SignPartial();

        if (!builder.GetPeerSignature())
        {
            if (subTxState == SubTxState::Initial && !isTxOwner)
            {
                // invited participant
                assert(IsInitiator());
                ConfirmSharedTxInvitation(builder);
                SetState(SubTxState::Constructed, subTxID);
                subTxState = SubTxState::Constructed;
            }
            return subTxState;
        }

        if (!builder.IsPeerSignatureValid())
        {
            LOG_INFO() << GetTxID() << " Peer signature is invalid.";
            return subTxState;
        }

        builder.FinalizeSignature();

        SetState(SubTxState::Constructed, subTxID);
        subTxState = SubTxState::Constructed;

        if (isTxOwner)
        {
            auto transaction = builder.CreateTransaction();
            beam::TxBase::Context context;
            if (!transaction->IsValid(context))
            {
                // TODO: check
                OnFailed(TxFailureReason::InvalidTransaction, true);
                return subTxState;
            }

            m_RedeemTx = transaction;
        }

        return subTxState;
    }

    bool AtomicSwapTransaction::SendSubTx(Transaction::Ptr transaction, SubTxID subTxID)
    {
        bool isRegistered = false;
        if (!GetParameter(TxParameterID::TransactionRegistered, isRegistered, subTxID))
        {
            m_Gateway.register_tx(GetTxID(), transaction);
            return isRegistered;
        }

        if (!isRegistered)
        {
            OnFailed(TxFailureReason::FailedToRegister, true);
            return isRegistered;
        }

        return isRegistered;
    }

    bool AtomicSwapTransaction::IsBeamLockTimeExpired() const
    {
        Height lockTimeHeight = MaxHeight;
        GetParameter(TxParameterID::MinHeight, lockTimeHeight);

        Block::SystemState::Full state;

        return GetTip(state) && state.m_Height > (lockTimeHeight + kBeamLockTimeInBlocks);
    }

    bool AtomicSwapTransaction::IsSubTxCompleted(SubTxID subTxID) const
    {
        Height hProof = 0;
        // TODO: check
        GetParameter(TxParameterID::KernelProofHeight, hProof/*, subTxID*/);
        if (!hProof)
        {
            Merkle::Hash kernelID = GetMandatoryParameter<Merkle::Hash>(TxParameterID::KernelID, subTxID);
            m_Gateway.confirm_kernel(GetTxID(), kernelID);
            return false;
        }
        return true;
    }

    bool AtomicSwapTransaction::GetPreimageFromChain(ECC::uintBig& preimage) const
    {
        Height hProof = 0;
        GetParameter(TxParameterID::KernelProofHeight, hProof/*, subTxID*/);
        GetParameter(TxParameterID::PreImage, preimage);

        if (!hProof)
        {
            Merkle::Hash kernelID = GetMandatoryParameter<Merkle::Hash>(TxParameterID::KernelID, SubTxIndex::BEAM_REDEEM_TX);
            m_Gateway.get_kernel(GetTxID(), kernelID);
            return false;
        }

        return true;
    }

    Amount AtomicSwapTransaction::GetAmount() const
    {
        if (!m_Amount.is_initialized())
        {
            m_Amount = GetMandatoryParameter<Amount>(TxParameterID::Amount);
        }
        return *m_Amount;
    }

    bool AtomicSwapTransaction::IsSender() const
    {
        if (!m_IsSender.is_initialized())
        {
            m_IsSender = GetMandatoryParameter<bool>(TxParameterID::IsSender);
        }
        return *m_IsSender;
    }

    void AtomicSwapTransaction::SendInvitation()
    {
        Amount swapAmount = GetMandatoryParameter<Amount>(TxParameterID::AtomicSwapAmount);
        AtomicSwapCoin swapCoin = GetMandatoryParameter<AtomicSwapCoin>(TxParameterID::AtomicSwapCoin);
        std::string swapAddress = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapAddress);

        // send invitation
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::Amount, GetAmount())
            .AddParameter(TxParameterID::IsSender, !IsSender())
            .AddParameter(TxParameterID::AtomicSwapAmount, swapAmount)
            .AddParameter(TxParameterID::AtomicSwapCoin, swapCoin)
            .AddParameter(TxParameterID::AtomicSwapPeerAddress, swapAddress)
            .AddParameter(TxParameterID::PeerProtoVersion, s_ProtoVersion);

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendExternalTxDetails()
    {
        auto txID = GetMandatoryParameter<std::string>(TxParameterID::AtomicSwapExternalTxID, SubTxIndex::LOCK_TX);
        int outputIndex = GetMandatoryParameter<int>(TxParameterID::AtomicSwapExternalTxOutputIndex, SubTxIndex::LOCK_TX);

        SetTxParameter msg;
        msg.AddParameter(TxParameterID::SubTxIndex, SubTxIndex::LOCK_TX)
            .AddParameter(TxParameterID::AtomicSwapExternalTxID, txID)
            .AddParameter(TxParameterID::AtomicSwapExternalTxOutputIndex, outputIndex);

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendLockTxInvitation(const LockTxBuilder& lockBuilder, bool isSender)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::Fee, lockBuilder.GetFee())
            .AddParameter(TxParameterID::SubTxIndex, SubTxIndex::BEAM_LOCK_TX)
            .AddParameter(TxParameterID::MinHeight, lockBuilder.GetMinHeight())
            .AddParameter(TxParameterID::PeerPublicExcess, lockBuilder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerPublicNonce, lockBuilder.GetPublicNonce());

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendBulletProofPart2(const LockTxBuilder& lockBuilder, bool isSender)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::SubTxIndex, SubTxIndex::BEAM_LOCK_TX)
            .AddParameter(TxParameterID::PeerSignature, lockBuilder.GetPartialSignature())
            .AddParameter(TxParameterID::PeerOffset, lockBuilder.GetOffset())
            .AddParameter(TxParameterID::PeerPublicSharedBlindingFactor, lockBuilder.GetPublicSharedBlindingFactor());
        if (isSender)
        {
            auto proofPartialMultiSig = lockBuilder.GetProofPartialMultiSig();
            msg.AddParameter(TxParameterID::PeerSharedBulletProofMSig, proofPartialMultiSig);
        }
        else
        {
            auto bulletProof = lockBuilder.GetSharedProof();
            msg.AddParameter(TxParameterID::PeerPublicExcess, lockBuilder.GetPublicExcess())
                .AddParameter(TxParameterID::PeerPublicNonce, lockBuilder.GetPublicNonce())
                .AddParameter(TxParameterID::PeerSharedBulletProofPart2, bulletProof.m_Part2);
        }

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendBulletProofPart3(const LockTxBuilder& lockBuilder, bool isSender)
    {
        SetTxParameter msg;

        msg.AddParameter(TxParameterID::SubTxIndex, SubTxIndex::BEAM_LOCK_TX);
        // if !isSender -> send p3
        // else send full bulletproof? output?

        if (!isSender)
        {
            auto bulletProof = lockBuilder.GetSharedProof();
            msg.AddParameter(TxParameterID::PeerSharedBulletProofPart3, bulletProof.m_Part3);
        }

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::SendSharedTxInvitation(const BaseTxBuilder& builder, bool shouldSendLockImage /*= false*/)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::SubTxIndex, builder.GetSubTxID())
            .AddParameter(TxParameterID::Amount, builder.GetAmount())
            .AddParameter(TxParameterID::Fee, builder.GetFee())
            .AddParameter(TxParameterID::MinHeight, builder.GetMinHeight())
            .AddParameter(TxParameterID::PeerPublicExcess, builder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerPublicNonce, builder.GetPublicNonce());
    
        if (shouldSendLockImage)
        {
            msg.AddParameter(TxParameterID::PeerLockImage, builder.GetLockImage());
        }

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::ConfirmSharedTxInvitation(const BaseTxBuilder& builder)
    {
        SetTxParameter msg;
        msg.AddParameter(TxParameterID::SubTxIndex, builder.GetSubTxID())
            .AddParameter(TxParameterID::PeerPublicExcess, builder.GetPublicExcess())
            .AddParameter(TxParameterID::PeerSignature, builder.GetPartialSignature())
            .AddParameter(TxParameterID::PeerPublicNonce, builder.GetPublicNonce())
            .AddParameter(TxParameterID::PeerOffset, builder.GetOffset());

        if (!SendTxParameters(move(msg)))
        {
            OnFailed(TxFailureReason::FailedToSendParameters, false);
        }
    }

    void AtomicSwapTransaction::OnGetRawChangeAddress(const std::string& response)
    {
        assert(!response.empty());

        json reply = json::parse(response);

        LOG_DEBUG() << reply.dump(4);

        // TODO: validate error
        // const auto& error = reply["error"];

        const auto& result = reply["result"];

        SetParameter(TxParameterID::AtomicSwapAddress, result.get<std::string>());
        SetNextState(State::Invitation);
    }

    void AtomicSwapTransaction::OnFundRawTransaction(const std::string& response)
    {
        assert(!response.empty());

        json reply = json::parse(response);
        LOG_DEBUG() << reply.dump(4);

        //const auto& error = reply["error"];
        const auto& result = reply["result"];
        auto hexTx = result["hex"].get<std::string>();
        int changePos = result["changepos"].get<int>();

        // float fee = result["fee"].get<float>();      // calculate fee!
        m_ValuePosition = changePos ? 0 : 1;

        SetParameter(TxParameterID::AtomicSwapExternalTxOutputIndex, m_ValuePosition, false, SubTxIndex::LOCK_TX);

        m_Gateway.get_bitcoin_rpc()->signRawTransaction(hexTx, BIND_THIS_MEMFN(OnSignLockTransaction));
    }

    void AtomicSwapTransaction::OnSignLockTransaction(const std::string& response)
    {
        assert(!response.empty());
        json reply = json::parse(response);
        LOG_DEBUG() << reply.dump(4);

        //const auto& error = reply["error"];
        LOG_DEBUG() << reply["result"]["hex"].get<std::string>();
        const auto& result = reply["result"];

        assert(result["complete"].get<bool>());
        m_SwapLockRawTx = result["hex"].get<std::string>();

        SetState(SwapTxState::Constructed, SubTxIndex::LOCK_TX);
        UpdateAsync();
    }

    void AtomicSwapTransaction::OnCreateRefundTransaction(const std::string& response)
    {
        assert(!response.empty());
        json reply = json::parse(response);
        m_SwapWithdrawRawTx = reply["result"].get<std::string>();
        UpdateAsync();
    }

    void AtomicSwapTransaction::OnDumpSenderPrivateKey(const std::string& response)
    {
        assert(!response.empty());
        // Parse reply
        json reply = json::parse(response);
        const auto& result = reply["result"];

        libbitcoin::data_chunk tx_data;
        libbitcoin::decode_base16(tx_data, *m_SwapWithdrawRawTx);
        libbitcoin::chain::transaction withdrawTX = libbitcoin::chain::transaction::factory_from_data(tx_data);

        libbitcoin::wallet::ec_private wallet_key(result.get<std::string>(), libbitcoin::wallet::ec_private::testnet_wif);
        libbitcoin::endorsement sig;

        uint32_t input_index = 0;
        auto redeemScript = CreateAtomicSwapContract(*this);
        libbitcoin::chain::script::create_endorsement(sig, wallet_key.secret(), redeemScript, withdrawTX, input_index, libbitcoin::machine::sighash_algorithm::all);

        // Create input script
        libbitcoin::machine::operation::list sig_script;
        libbitcoin::ec_compressed pubkey = wallet_key.to_public().point();

        // <my sig> <my pubkey> 0
        sig_script.push_back(libbitcoin::machine::operation(sig));
        sig_script.push_back(libbitcoin::machine::operation(libbitcoin::to_chunk(pubkey)));
        sig_script.push_back(libbitcoin::machine::operation(libbitcoin::machine::opcode(0)));

        libbitcoin::chain::script input_script(sig_script);

        // Add input script to first input in transaction
        withdrawTX.inputs()[0].set_script(input_script);

        // update m_SwapWithdrawRawTx
        m_SwapWithdrawRawTx = libbitcoin::encode_base16(withdrawTX.to_data());
        
        SetState(SwapTxState::Constructed, SubTxIndex::REFUND_TX);
        UpdateAsync();
    }

    void AtomicSwapTransaction::OnDumpReceiverPrivateKey(const std::string& response)
    {
        assert(!response.empty());
        // Parse reply
        json reply = json::parse(response);
        const auto& result = reply["result"];

        libbitcoin::data_chunk tx_data;
        libbitcoin::decode_base16(tx_data, *m_SwapWithdrawRawTx);
        libbitcoin::chain::transaction withdrawTX = libbitcoin::chain::transaction::factory_from_data(tx_data);

        libbitcoin::wallet::ec_private wallet_key(result.get<std::string>(), libbitcoin::wallet::ec_private::testnet_wif);
        libbitcoin::endorsement sig;

        uint32_t input_index = 0;
        auto redeemScript = CreateAtomicSwapContract(*this);
        libbitcoin::chain::script::create_endorsement(sig, wallet_key.secret(), redeemScript, withdrawTX, input_index, libbitcoin::machine::sighash_algorithm::all);

        // Create input script
        libbitcoin::machine::operation::list sig_script;
        libbitcoin::ec_compressed pubkey = wallet_key.to_public().point();

        auto secret = GetMandatoryParameter<ECC::uintBig>(TxParameterID::PreImage);

        // <their sig> <their pubkey> <initiator secret> 1
        sig_script.push_back(libbitcoin::machine::operation(sig));
        sig_script.push_back(libbitcoin::machine::operation(libbitcoin::to_chunk(pubkey)));
        sig_script.push_back(libbitcoin::machine::operation(libbitcoin::to_chunk(secret.m_pData)));
        sig_script.push_back(libbitcoin::machine::operation(libbitcoin::machine::opcode::push_positive_1));

        libbitcoin::chain::script input_script(sig_script);

        // Add input script to first input in transaction
        withdrawTX.inputs()[0].set_script(input_script);

        // update m_SwapWithdrawRawTx
        m_SwapWithdrawRawTx = libbitcoin::encode_base16(withdrawTX.to_data());

        SetState(SwapTxState::Constructed, SubTxIndex::REDEEM_TX);
        UpdateAsync();
    }

    void AtomicSwapTransaction::OnGetSwapLockTxConfirmations(const std::string& response)
    {
        if (response.empty())
        {
            return;
        }

        json reply = json::parse(response);

        // get confirmations
        m_SwapLockTxConfirmations = reply["result"]["confirmations"];

        // TODO: validate contract!        

        if (m_SwapLockTxConfirmations >= kBTCMinTxConfirmations)
        {
            UpdateAsync();
        }
    }

    LockTxBuilder::LockTxBuilder(BaseTransaction& tx, Amount amount, Amount fee)
        : BaseTxBuilder(tx, AtomicSwapTransaction::SubTxIndex::BEAM_LOCK_TX, {amount}, fee)
    {
    }

    void LockTxBuilder::LoadPeerOffset()
    {
        m_Tx.GetParameter(TxParameterID::PeerOffset, m_PeerOffset, m_SubTxID);
    }

    void LockTxBuilder::SharedUTXOProofPart2(bool shouldProduceMultisig)
    {
        if (shouldProduceMultisig)
        {
            Oracle oracle;
            oracle << (beam::Height)0; // CHECK, coin maturity
            // load peer part2
            m_Tx.GetParameter(TxParameterID::PeerSharedBulletProofPart2, m_SharedProof.m_Part2, m_SubTxID);
            // produce multisig
            m_SharedProof.CoSign(GetSharedSeed(), GetSharedBlindingFactor(), GetProofCreatorParams(), oracle, RangeProof::Confidential::Phase::Step2, &m_ProofPartialMultiSig);

            // save SharedBulletProofMSig and BulletProof ?
            m_Tx.SetParameter(TxParameterID::SharedBulletProof, m_SharedProof, m_SubTxID);
        }
        else
        {
            ZeroObject(m_SharedProof.m_Part2);
            RangeProof::Confidential::MultiSig::CoSignPart(GetSharedSeed(), m_SharedProof.m_Part2);
        }
    }

    void LockTxBuilder::SharedUTXOProofPart3(bool shouldProduceMultisig)
    {
        if (shouldProduceMultisig)
        {
            Oracle oracle;
            oracle << (beam::Height)0; // CHECK!
            // load peer part3
            m_Tx.GetParameter(TxParameterID::PeerSharedBulletProofPart3, m_SharedProof.m_Part3, m_SubTxID);
            // finalize proof
            m_SharedProof.CoSign(GetSharedSeed(), GetSharedBlindingFactor(), GetProofCreatorParams(), oracle, RangeProof::Confidential::Phase::Finalize);

            m_Tx.SetParameter(TxParameterID::SharedBulletProof, m_SharedProof, m_SubTxID);
        }
        else
        {
            m_Tx.GetParameter(TxParameterID::PeerSharedBulletProofMSig, m_ProofPartialMultiSig, m_SubTxID);

            ZeroObject(m_SharedProof.m_Part3);
            m_ProofPartialMultiSig.CoSignPart(GetSharedSeed(), GetSharedBlindingFactor(), m_SharedProof.m_Part3);
        }
    }

    void LockTxBuilder::AddSharedOutput()
    {
        Output::Ptr output = make_unique<Output>();
        output->m_Commitment = GetSharedCommitment();
        output->m_pConfidential = std::make_unique<ECC::RangeProof::Confidential>();
        *(output->m_pConfidential) = m_SharedProof;

        m_Outputs.push_back(std::move(output));
    }

    void LockTxBuilder::LoadSharedParameters()
    {
        if (!m_Tx.GetParameter(TxParameterID::SharedBlindingFactor, m_SharedBlindingFactor, m_SubTxID))
        {
            m_SharedCoin = m_Tx.GetWalletDB()->generateSharedCoin(GetAmount());
            m_Tx.SetParameter(TxParameterID::SharedCoinID, m_SharedCoin.m_ID, m_SubTxID);

            // blindingFactor = sk + sk1
            beam::SwitchCommitment switchCommitment;
            switchCommitment.Create(m_SharedBlindingFactor, *m_Tx.GetWalletDB()->get_ChildKdf(m_SharedCoin.m_ID.m_SubIdx), m_SharedCoin.m_ID);
            m_Tx.SetParameter(TxParameterID::SharedBlindingFactor, m_SharedBlindingFactor, m_SubTxID);

            Oracle oracle;
            RangeProof::Confidential::GenerateSeed(m_SharedSeed.V, m_SharedBlindingFactor, GetAmount(), oracle);
            m_Tx.SetParameter(TxParameterID::SharedSeed, m_SharedSeed.V, m_SubTxID);
        }
        else
        {
            // load remaining shared parameters
            m_Tx.GetParameter(TxParameterID::SharedSeed, m_SharedSeed.V, m_SubTxID);
            m_Tx.GetParameter(TxParameterID::SharedCoinID, m_SharedCoin.m_ID, m_SubTxID);
            m_Tx.GetParameter(TxParameterID::SharedBulletProof, m_SharedProof, m_SubTxID);
        }

        ECC::Scalar::Native blindingFactor = -m_SharedBlindingFactor;
        m_Offset += blindingFactor;
    }

    Transaction::Ptr LockTxBuilder::CreateTransaction()
    {
        AddSharedOutput();
        LoadPeerOffset();
        return BaseTxBuilder::CreateTransaction();
    }

    const ECC::uintBig& LockTxBuilder::GetSharedSeed() const
    {
        return m_SharedSeed.V;
    }

    const ECC::Scalar::Native& LockTxBuilder::GetSharedBlindingFactor() const
    {
        return m_SharedBlindingFactor;
    }

    const ECC::RangeProof::Confidential& LockTxBuilder::GetSharedProof() const
    {
        return m_SharedProof;
    }

    const ECC::RangeProof::Confidential::MultiSig& LockTxBuilder::GetProofPartialMultiSig() const
    {
        return m_ProofPartialMultiSig;
    }

    ECC::Point::Native LockTxBuilder::GetPublicSharedBlindingFactor() const
    {
        return Context::get().G * GetSharedBlindingFactor();
    }

    const ECC::RangeProof::CreatorParams& LockTxBuilder::GetProofCreatorParams()
    {
        if (!m_CreatorParams.is_initialized())
        {
            ECC::RangeProof::CreatorParams creatorParams;
            creatorParams.m_Kidv = m_SharedCoin.m_ID;
            beam::Output::GenerateSeedKid(creatorParams.m_Seed.V, GetSharedCommitment(), *m_Tx.GetWalletDB()->get_MasterKdf());
            m_CreatorParams = creatorParams;
        }
        return m_CreatorParams.get();
    }

    ECC::Point::Native LockTxBuilder::GetSharedCommitment()
    {
        Point::Native commitment(Zero);
        // TODO: check pHGen
        Tag::AddValue(commitment, nullptr, GetAmount());
        commitment += GetPublicSharedBlindingFactor();
        commitment += m_Tx.GetMandatoryParameter<Point::Native>(TxParameterID::PeerPublicSharedBlindingFactor, m_SubTxID);

        return commitment;
    }

    SharedTxBuilder::SharedTxBuilder(BaseTransaction& tx, SubTxID subTxID, Amount amount, Amount fee)
        : BaseTxBuilder(tx, subTxID, { amount }, fee)
    {
    }

    Transaction::Ptr SharedTxBuilder::CreateTransaction()
    {
        LoadPeerOffset();
        return BaseTxBuilder::CreateTransaction();
    }

    bool SharedTxBuilder::GetSharedParameters()
    {
        return m_Tx.GetParameter(TxParameterID::SharedBlindingFactor, m_SharedBlindingFactor, AtomicSwapTransaction::SubTxIndex::BEAM_LOCK_TX)
            && m_Tx.GetParameter(TxParameterID::PeerPublicSharedBlindingFactor, m_PeerPublicSharedBlindingFactor, AtomicSwapTransaction::SubTxIndex::BEAM_LOCK_TX);
    }

    void SharedTxBuilder::InitTx(bool isTxOwner)
    {
        if (isTxOwner)
        {
            // select shared UTXO as input and create output utxo
            InitInputAndOutputs();

            if (!FinalizeOutputs())
            {
                // TODO: transaction is too big :(
            }
        }
        else
        {
            // init offset
            InitOffset();
        }
    }

    void SharedTxBuilder::InitInputAndOutputs()
    {
        // load shared utxo as input

        // TODO: move it to separate function
        Point::Native commitment(Zero);
        Tag::AddValue(commitment, nullptr, GetAmount());
        commitment += Context::get().G * m_SharedBlindingFactor;
        commitment += m_PeerPublicSharedBlindingFactor;

        auto& input = m_Inputs.emplace_back(make_unique<Input>());
        input->m_Commitment = commitment;
        m_Tx.SetParameter(TxParameterID::Inputs, m_Inputs, false, m_SubTxID);

        m_Offset += m_SharedBlindingFactor;

        // add output
        AddOutput(GetAmount(), false);
    }

    void SharedTxBuilder::InitOffset()
    {
        m_Offset += m_SharedBlindingFactor;
        m_Tx.SetParameter(TxParameterID::Offset, m_Offset, false, m_SubTxID);
    }

    void SharedTxBuilder::LoadPeerOffset()
    {
        m_Tx.GetParameter(TxParameterID::PeerOffset, m_PeerOffset, m_SubTxID);
    }
} // namespace