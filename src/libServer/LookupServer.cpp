/*
 * Copyright (C) 2019 Zilliqa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "LookupServer.h"
#include <boost/multiprecision/cpp_dec_float.hpp>
#include "JSONConversion.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "libCrypto/Schnorr.h"
#include "libCrypto/Sha2.h"
#include "libData/AccountData/Account.h"
#include "libData/AccountData/AccountStore.h"
#include "libData/AccountData/Transaction.h"
#include "libMessage/Messenger.h"
#include "libNetwork/Blacklist.h"
#include "libNetwork/P2PComm.h"
#include "libNetwork/Peer.h"
#include "libPersistence/BlockStorage.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/TimeUtils.h"

using namespace jsonrpc;
using namespace std;

CircularArray<std::string> LookupServer::m_RecentTransactions;
std::mutex LookupServer::m_mutexRecentTxns;

const unsigned int PAGE_SIZE = 10;
const unsigned int NUM_PAGES_CACHE = 2;
const unsigned int TXN_PAGE_SIZE = 100;

//[warning] do not make this constant too big as it loops over blockchain
const unsigned int REF_BLOCK_DIFF = 1;

LookupServer::LookupServer(Mediator& mediator,
                           jsonrpc::AbstractServerConnector& server)
    : Server(mediator),
      jsonrpc::AbstractServer<LookupServer>(server,
                                            jsonrpc::JSONRPC_SERVER_V2) {
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetCurrentMiniEpoch", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &Server::GetCurrentMiniEpochI);

  this->bindAndAddMethod(
      jsonrpc::Procedure("GetCurrentDSEpoch", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &Server::GetCurrentDSEpochI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNodeType", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &Server::GetNodeTypeI);

  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNetworkId", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetNetworkIdI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("CreateTransaction", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_OBJECT,
                         NULL),
      &LookupServer::CreateTransactionI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetTransaction", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetTransactionI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetDsBlock", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetDsBlockI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetTxBlock", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetTxBlockI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetLatestDsBlock", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, NULL),
      &LookupServer::GetLatestDsBlockI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetLatestTxBlock", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, NULL),
      &LookupServer::GetLatestTxBlockI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetBalance", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetBalanceI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetMinimumGasPrice", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetMinimumGasPriceI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetPrevDSDifficulty", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_INTEGER, NULL),
      &Server::GetPrevDSDifficultyI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetPrevDifficulty", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_INTEGER, NULL),
      &Server::GetPrevDifficultyI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetSmartContracts", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_ARRAY, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetSmartContractsI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetContractAddressFromTransactionID",
                         jsonrpc::PARAMS_BY_POSITION, jsonrpc::JSON_STRING,
                         "param01", jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetContractAddressFromTransactionIDI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNumPeers", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_INTEGER, NULL),
      &LookupServer::GetNumPeersI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNumTxBlocks", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetNumTxBlocksI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNumDSBlocks", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetNumDSBlocksI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNumTransactions", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetNumTransactionsI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetTransactionRate", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_REAL, NULL),
      &LookupServer::GetTransactionRateI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetTxBlockRate", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_REAL, NULL),
      &LookupServer::GetTxBlockRateI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetDSBlockRate", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_REAL, NULL),
      &LookupServer::GetDSBlockRateI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetShardMembers", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_INTEGER,
                         NULL),
      &LookupServer::GetShardMembersI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("DSBlockListing", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_INTEGER,
                         NULL),
      &LookupServer::DSBlockListingI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("TxBlockListing", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_INTEGER,
                         NULL),
      &LookupServer::TxBlockListingI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetBlockchainInfo", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, NULL),
      &LookupServer::GetBlockchainInfoI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetRecentTransactions", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, NULL),
      &LookupServer::GetRecentTransactionsI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetShardingStructure", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, NULL),
      &LookupServer::GetShardingStructureI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNumTxnsTxEpoch", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetNumTxnsTxEpochI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetNumTxnsDSEpoch", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetNumTxnsDSEpochI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetSmartContractState", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetSmartContractStateI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetSmartContractCode", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetSmartContractCodeI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetSmartContractInit", jsonrpc::PARAMS_BY_POSITION,
                         jsonrpc::JSON_OBJECT, "param01", jsonrpc::JSON_STRING,
                         NULL),
      &LookupServer::GetSmartContractInitI);
  this->bindAndAddMethod(
      jsonrpc::Procedure("GetTransactionsForTxBlock",
                         jsonrpc::PARAMS_BY_POSITION, jsonrpc::JSON_OBJECT,
                         "param01", jsonrpc::JSON_STRING, NULL),
      &LookupServer::GetTransactionsForTxBlockI);

  m_StartTimeTx = 0;
  m_StartTimeDs = 0;
  m_DSBlockCache.first = 0;
  m_DSBlockCache.second.resize(NUM_PAGES_CACHE * PAGE_SIZE);
  m_TxBlockCache.first = 0;
  m_TxBlockCache.second.resize(NUM_PAGES_CACHE * PAGE_SIZE);
  m_RecentTransactions.resize(TXN_PAGE_SIZE);
  m_TxBlockCountSumPair.first = 0;
  m_TxBlockCountSumPair.second = 0;
  random_device rd;
  m_eng = mt19937(rd());
}

string LookupServer::GetNetworkId() {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  return to_string(CHAIN_ID);
}

bool LookupServer::StartCollectorThread() {
  if (!LOOKUP_NODE_MODE || !ARCHIVAL_LOOKUP) {
    LOG_GENERAL(
        WARNING,
        "Not expected to be called from node other than LOOKUP ARCHIVAL ");
    return false;
  }
  auto collectorThread = [this]() mutable -> void {
    this_thread::sleep_for(chrono::seconds(POW_WINDOW_IN_SECONDS));

    vector<Transaction> txns;
    LOG_GENERAL(INFO, "[ARCHLOOK]"
                          << "Start thread");
    while (true) {
      this_thread::sleep_for(chrono::seconds(SEED_TXN_COLLECTION_TIME_IN_SEC));
      txns.clear();

      if (m_mediator.m_lookup->GetSyncType() == SyncType::NEW_LOOKUP_SYNC) {
        LOG_GENERAL(INFO, "This new lookup (Seed) is not yet synced..");
        continue;
      }

      if (USE_REMOTE_TXN_CREATOR && !m_mediator.m_lookup->GenTxnToSend(
                                        NUM_TXN_TO_SEND_PER_ACCOUNT, txns)) {
        LOG_GENERAL(WARNING, "GenTxnToSend failed");
      }

      if (!txns.empty()) {
        for (const auto& tx : txns) {
          m_mediator.m_lookup->AddToTxnShardMap(tx, 0);
        }
      }
      // LOG_GENERAL(INFO, "Size of txns " << txns.size());

      bool hasTxn = false;

      for (auto const& i :
           {SEND_TYPE::ARCHIVAL_SEND_SHARD, SEND_TYPE::ARCHIVAL_SEND_DS}) {
        {
          lock_guard<mutex> g(m_mediator.m_lookup->m_txnShardMapMutex);
          if (m_mediator.m_lookup->GetTxnFromShardMap(i).empty()) {
            continue;
          }
          hasTxn = true;
        }
      }

      if (!hasTxn) {
        LOG_GENERAL(INFO, "No Txns to send for this seed node");
        continue;
      }

      bytes msg = {MessageType::LOOKUP, LookupInstructionType::FORWARDTXN};

      {
        lock_guard<mutex> g(m_mediator.m_lookup->m_txnShardMapMutex);
        if (!Messenger::SetForwardTxnBlockFromSeed(
                msg, MessageOffset::BODY,
                m_mediator.m_lookup->GetTxnFromShardMap(
                    SEND_TYPE::ARCHIVAL_SEND_SHARD),
                m_mediator.m_lookup->GetTxnFromShardMap(
                    SEND_TYPE::ARCHIVAL_SEND_DS))) {
          continue;
        }
      }

      m_mediator.m_lookup->SendMessageToRandomSeedNode(msg);

      for (auto const& i :
           {SEND_TYPE::ARCHIVAL_SEND_SHARD, SEND_TYPE::ARCHIVAL_SEND_DS}) {
        m_mediator.m_lookup->DeleteTxnShardMap(i);
      }
    }
  };
  DetachedFunction(1, collectorThread);
  return true;
}

bool LookupServer::ValidateTxn(const Transaction& tx, const Address& fromAddr,
                               const Account* sender) const {
  if (DataConversion::UnpackA(tx.GetVersion()) != CHAIN_ID) {
    throw JsonRpcException(RPC_VERIFY_REJECTED, "CHAIN_ID incorrect");
  }

  if (tx.GetCode().size() > MAX_CODE_SIZE_IN_BYTES) {
    throw JsonRpcException(RPC_VERIFY_REJECTED, "Code size is too large");
  }

  if (tx.GetGasPrice() <
      m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetGasPrice()) {
    throw JsonRpcException(RPC_VERIFY_REJECTED,
                           "GasPrice " + tx.GetGasPrice().convert_to<string>() +
                               " lower than minimum allowable " +
                               m_mediator.m_dsBlockChain.GetLastBlock()
                                   .GetHeader()
                                   .GetGasPrice()
                                   .convert_to<string>());
  }
  if (!m_mediator.m_validator->VerifyTransaction(tx)) {
    throw JsonRpcException(RPC_VERIFY_REJECTED, "Unable to verify transaction");
  }

  unsigned int num_shards = m_mediator.m_lookup->GetShardPeers().size();

  if (fromAddr == Address()) {
    throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                           "Invalid address for issuing transactions");
  }

  if (sender == nullptr) {
    throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                           "The sender of the txn has no balance");
  }

  if (num_shards == 0) {
    throw JsonRpcException(RPC_IN_WARMUP, "No Shards yet");
  }

  return true;
}

Json::Value LookupServer::CreateTransaction(const Json::Value& _json) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    if (!JSONConversion::checkJsonTx(_json)) {
      throw JsonRpcException(RPC_PARSE_ERROR, "Invalid Transaction JSON");
    }

    Transaction tx = JSONConversion::convertJsontoTx(_json);

    Json::Value ret;

    const Address fromAddr = tx.GetSenderAddr();
    const Account* sender = AccountStore::GetInstance().GetAccount(fromAddr);

    if (!ValidateTxn(tx, fromAddr, sender)) {
      return ret;
    }

    const unsigned int num_shards = m_mediator.m_lookup->GetShardPeers().size();
    const unsigned int shard = Transaction::GetShardIndex(fromAddr, num_shards);
    unsigned int mapIndex = shard;
    switch (Transaction::GetTransactionType(tx)) {
      case Transaction::ContractType::NON_CONTRACT:
        if (ARCHIVAL_LOOKUP) {
          mapIndex = SEND_TYPE::ARCHIVAL_SEND_SHARD;
        }
        ret["Info"] = "Non-contract txn, sent to shard";
        break;
      case Transaction::ContractType::CONTRACT_CREATION:
        if (!ENABLE_SC) {
          throw JsonRpcException(RPC_MISC_ERROR, "Smart contract is disabled");
        }
        if (ARCHIVAL_LOOKUP) {
          mapIndex = SEND_TYPE::ARCHIVAL_SEND_SHARD;
        }
        ret["Info"] = "Contract Creation txn, sent to shard";
        ret["ContractAddress"] =
            Account::GetAddressForContract(fromAddr, sender->GetNonce()).hex();
        break;
      case Transaction::ContractType::CONTRACT_CALL: {
        if (!ENABLE_SC) {
          throw JsonRpcException(RPC_MISC_ERROR, "Smart contract is disabled");
        }
        const Account* account =
            AccountStore::GetInstance().GetAccount(tx.GetToAddr());

        if (account == nullptr) {
          throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY, "To addr is null");
        }

        else if (!account->isContract()) {
          throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                                 "Non - contract address called");
        }

        unsigned int to_shard =
            Transaction::GetShardIndex(tx.GetToAddr(), num_shards);
        bool sendToDs = false;
        if (_json.isMember("priority")) {
          sendToDs = _json["priority"].asBool();
        }
        if ((to_shard == shard) && !sendToDs) {
          if (ARCHIVAL_LOOKUP) {
            mapIndex = SEND_TYPE::ARCHIVAL_SEND_SHARD;
          }
          ret["Info"] =
              "Contract Txn, Shards Match of the sender "
              "and reciever";
        } else {
          if (ARCHIVAL_LOOKUP) {
            mapIndex = SEND_TYPE::ARCHIVAL_SEND_DS;
          } else {
            mapIndex = num_shards;
          }
          ret["Info"] = "Contract Txn, Sent To Ds";
        }
      } break;
      case Transaction::ContractType::ERROR:
        throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                               "Code is empty and To addr is null");
        break;
      default:
        throw JsonRpcException(RPC_MISC_ERROR, "Txn type unexpected");
    }
    if (!m_mediator.m_lookup->AddToTxnShardMap(tx, mapIndex)) {
      throw JsonRpcException(RPC_DATABASE_ERROR,
                             "Txn could not be added as database exceeded "
                             "limit or the txn was already present");
    }
    ret["TranID"] = tx.GetTranID().hex();
    return ret;
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO,
                "[Error]" << e.what() << " Input: " << _json.toStyledString());
    throw JsonRpcException(RPC_MISC_ERROR, "Unable to Process");
  }
}

Json::Value LookupServer::GetTransaction(const string& transactionHash) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    TxBodySharedPtr tptr;
    TxnHash tranHash(transactionHash);
    if (transactionHash.size() != TRAN_HASH_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMS, "Size not appropriate");
    }
    bool isPresent = BlockStorage::GetBlockStorage().GetTxBody(tranHash, tptr);
    bool isPresentHistorical = false;
    if (m_mediator.m_lookup->m_historicalDB && !isPresent) {
      isPresentHistorical =
          BlockStorage::GetBlockStorage().GetTxnFromHistoricalDB(tranHash,
                                                                 tptr);
    }
    if (isPresentHistorical || isPresent) {
      Json::Value _json;
      return JSONConversion::convertTxtoJson(*tptr);
    } else {
      throw JsonRpcException(RPC_DATABASE_ERROR, "Txn Hash not Present");
    }
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << transactionHash);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable to Process");
  }
}

Json::Value LookupServer::GetDsBlock(const string& blockNum) {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    uint64_t BlockNum = stoull(blockNum);
    return JSONConversion::convertDSblocktoJson(
        m_mediator.m_dsBlockChain.GetBlock(BlockNum));
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (runtime_error& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_INVALID_PARAMS, "String not numeric");
  } catch (invalid_argument& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_INVALID_PARAMS, "Invalid arugment");
  } catch (out_of_range& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_INVALID_PARAMS, "Out of range");
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

Json::Value LookupServer::GetTxBlock(const string& blockNum) {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    uint64_t BlockNum = stoull(blockNum);
    return JSONConversion::convertTxBlocktoJson(
        m_mediator.m_txBlockChain.GetBlock(BlockNum));
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (runtime_error& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_INVALID_PARAMS, "String not numeric");
  } catch (invalid_argument& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_INVALID_PARAMS, "Invalid arugment");
  } catch (out_of_range& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_INVALID_PARAMS, "Out of range");
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << blockNum);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

string LookupServer::GetMinimumGasPrice() {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  return m_mediator.m_dsBlockChain.GetLastBlock()
      .GetHeader()
      .GetGasPrice()
      .str();
}

Json::Value LookupServer::GetLatestDsBlock() {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  LOG_MARKER();
  DSBlock Latest = m_mediator.m_dsBlockChain.GetLastBlock();

  LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
            "BlockNum " << Latest.GetHeader().GetBlockNum()
                        << "  Timestamp:        " << Latest.GetTimestamp());

  return JSONConversion::convertDSblocktoJson(Latest);
}

Json::Value LookupServer::GetLatestTxBlock() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  TxBlock Latest = m_mediator.m_txBlockChain.GetLastBlock();

  LOG_EPOCH(INFO, m_mediator.m_currentEpochNum,
            "BlockNum " << Latest.GetHeader().GetBlockNum()
                        << "  Timestamp:        " << Latest.GetTimestamp());

  return JSONConversion::convertTxBlocktoJson(Latest);
}

Json::Value LookupServer::GetBalance(const string& address) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    if (address.size() != ACC_ADDR_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMETER,
                             "Address size not appropriate");
    }

    bytes tmpaddr;
    if (!DataConversion::HexStrToUint8Vec(address, tmpaddr)) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY, "invalid address");
    }
    Address addr(tmpaddr);
    const Account* account = AccountStore::GetInstance().GetAccount(addr);

    Json::Value ret;
    if (account != nullptr) {
      const uint128_t& balance = account->GetBalance();
      uint64_t nonce = account->GetNonce();

      ret["balance"] = balance.str();
      ret["nonce"] = static_cast<unsigned int>(nonce);
      LOG_GENERAL(INFO, "balance " << balance.str() << " nonce: " << nonce);
    } else if (account == nullptr) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Account is not created");
    }

    return ret;
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << address);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

Json::Value LookupServer::GetSmartContractState(const string& address) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    Json::Value _json;
    if (address.size() != ACC_ADDR_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMETER,
                             "Address size not appropriate");
    }
    bytes tmpaddr;
    if (!DataConversion::HexStrToUint8Vec(address, tmpaddr)) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY, "invalid address");
    }

    Address addr(tmpaddr);
    const Account* account = AccountStore::GetInstance().GetAccount(addr);

    if (account == nullptr) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address does not exist");
    }

    if (!account->isContract()) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address not contract address");
    }

    return account->GetStateJson(false);
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << address);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

Json::Value LookupServer::GetSmartContractInit(const string& address) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    Json::Value _json;
    if (address.size() != ACC_ADDR_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMETER,
                             "Address size not appropriate");
    }

    bytes tmpaddr;
    if (!DataConversion::HexStrToUint8Vec(address, tmpaddr)) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY, "invalid address");
    }
    Address addr(tmpaddr);
    const Account* account = AccountStore::GetInstance().GetAccount(addr);

    if (account == nullptr) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address does not exist");
    }
    if (!account->isContract()) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address not contract address");
    }

    return account->GetInitJson(false);
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << address);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

Json::Value LookupServer::GetSmartContractCode(const string& address) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    if (address.size() != ACC_ADDR_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMETER,
                             "Address size not appropriate");
    }
    bytes tmpaddr;
    if (!DataConversion::HexStrToUint8Vec(address, tmpaddr)) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY, "invalid address");
    }
    Address addr(tmpaddr);
    const Account* account = AccountStore::GetInstance().GetAccount(addr);

    if (account == nullptr) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address does not exist");
    }

    if (!account->isContract()) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address not contract address");
    }

    Json::Value _json;
    _json["code"] = DataConversion::CharArrayToString(account->GetCode());
    return _json;
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << address);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

Json::Value LookupServer::GetSmartContracts(const string& address) {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    if (address.size() != ACC_ADDR_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMETER,
                             "Address size not appropriate");
    }
    bytes tmpaddr;
    if (!DataConversion::HexStrToUint8Vec(address, tmpaddr)) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address is not a hex string");
    }

    Address addr(tmpaddr);
    const Account* account = AccountStore::GetInstance().GetAccount(addr);

    if (account == nullptr) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Address does not exist");
    }
    if (account->isContract()) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "A contract account queried");
    }
    uint64_t nonce = account->GetNonce();
    //[TODO] find out a more efficient way (using storage)
    Json::Value _json;

    for (uint64_t i = 0; i < nonce; i++) {
      Address contractAddr = Account::GetAddressForContract(addr, i);
      const Account* contractAccount =
          AccountStore::GetInstance().GetAccount(contractAddr);

      if (contractAccount == nullptr || !contractAccount->isContract()) {
        continue;
      }

      Json::Value tmpJson;
      tmpJson["address"] = contractAddr.hex();
      tmpJson["state"] = GetSmartContractState(contractAddr.hex());

      _json.append(tmpJson);
    }
    return _json;
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(INFO, "[Error]" << e.what() << " Input: " << address);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

string LookupServer::GetContractAddressFromTransactionID(const string& tranID) {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  try {
    TxBodySharedPtr tptr;
    TxnHash tranHash(tranID);
    if (tranID.size() != TRAN_HASH_SIZE * 2) {
      throw JsonRpcException(RPC_INVALID_PARAMETER,
                             "Address size not appropriate");
    }
    bool isPresent = BlockStorage::GetBlockStorage().GetTxBody(tranHash, tptr);
    if (!isPresent) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "Txn Hash not Present");
    }
    const Transaction& tx = tptr->GetTransaction();
    if (tx.GetCode().empty() || tx.GetToAddr() != NullAddress) {
      throw JsonRpcException(RPC_INVALID_ADDRESS_OR_KEY,
                             "ID is not a contract txn");
    }

    return Account::GetAddressForContract(tx.GetSenderAddr(), tx.GetNonce() - 1)
        .hex();
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(WARNING, "[Error]" << e.what() << " Input " << tranID);
    throw JsonRpcException(RPC_MISC_ERROR, "Unable To Process");
  }
}

unsigned int LookupServer::GetNumPeers() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  unsigned int numPeers = m_mediator.m_lookup->GetNodePeers().size();
  lock_guard<mutex> g(m_mediator.m_mutexDSCommittee);
  return numPeers + m_mediator.m_DSCommittee->size();
}

string LookupServer::GetNumTxBlocks() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  return to_string(m_mediator.m_txBlockChain.GetBlockCount());
}

string LookupServer::GetNumDSBlocks() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  return to_string(m_mediator.m_dsBlockChain.GetBlockCount());
}

string LookupServer::GetNumTransactions() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  lock_guard<mutex> g(m_mutexBlockTxPair);

  uint64_t currBlock =
      m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum();
  if (currBlock == INIT_BLOCK_NUMBER) {
    throw JsonRpcException(RPC_IN_WARMUP, "No Tx blocks");
  }
  if (m_BlockTxPair.first < currBlock) {
    for (uint64_t i = m_BlockTxPair.first + 1; i <= currBlock; i++) {
      m_BlockTxPair.second +=
          m_mediator.m_txBlockChain.GetBlock(i).GetHeader().GetNumTxs();
    }
  }
  m_BlockTxPair.first = currBlock;

  return m_BlockTxPair.second.str();
}

size_t LookupServer::GetNumTransactions(uint64_t blockNum) {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  uint64_t currBlockNum =
      m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum();

  if (currBlockNum == INIT_BLOCK_NUMBER) {
    throw JsonRpcException(RPC_IN_WARMUP, "No Tx blocks");
  }

  if (blockNum >= currBlockNum) {
    return 0;
  }

  size_t i, res = 0;

  for (i = blockNum + 1; i <= currBlockNum; i++) {
    res += m_mediator.m_txBlockChain.GetBlock(i).GetHeader().GetNumTxs();
  }

  return res;
}
double LookupServer::GetTransactionRate() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  uint64_t refBlockNum =
      m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum();

  uint64_t refTimeTx = 0;

  if (refBlockNum <= REF_BLOCK_DIFF) {
    if (refBlockNum <= 1) {
      LOG_GENERAL(INFO, "Not enough blocks for information");
      return 0;
    } else {
      refBlockNum = 1;
      // In case there are less than REF_DIFF_BLOCKS blocks in blockchain,
      // blocknum 1 can be ref block;
    }
  } else {
    refBlockNum = refBlockNum - REF_BLOCK_DIFF;
  }

  boost::multiprecision::cpp_dec_float_50 numTxns(
      LookupServer::GetNumTransactions(refBlockNum));
  LOG_GENERAL(INFO, "Num Txns: " << numTxns);

  try {
    TxBlock tx = m_mediator.m_txBlockChain.GetBlock(refBlockNum);
    refTimeTx = tx.GetTimestamp();
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (const char* msg) {
    if (string(msg) == "Blocknumber Absent") {
      LOG_GENERAL(INFO, "Error in fetching ref block");
    }
    return 0;
  }

  uint64_t TimeDiff =
      m_mediator.m_txBlockChain.GetLastBlock().GetTimestamp() - refTimeTx;

  if (TimeDiff == 0 || refTimeTx == 0) {
    // something went wrong
    LOG_GENERAL(INFO, "TimeDiff or refTimeTx = 0 \n TimeDiff:"
                          << TimeDiff << " refTimeTx:" << refTimeTx);
    return 0;
  }
  numTxns = numTxns * 1000000;  // conversion from microseconds to seconds
  boost::multiprecision::cpp_dec_float_50 TimeDiffFloat =
      static_cast<boost::multiprecision::cpp_dec_float_50>(TimeDiff);
  boost::multiprecision::cpp_dec_float_50 ans = numTxns / TimeDiffFloat;

  return ans.convert_to<double>();
}

double LookupServer::GetDSBlockRate() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  string numDSblockStr = to_string(m_mediator.m_dsBlockChain.GetBlockCount());
  boost::multiprecision::cpp_dec_float_50 numDs(numDSblockStr);

  if (m_StartTimeDs == 0)  // case when m_StartTime has not been set
  {
    try {
      // Refernce time chosen to be the first block's timestamp
      DSBlock dsb = m_mediator.m_dsBlockChain.GetBlock(1);
      m_StartTimeDs = dsb.GetTimestamp();
    } catch (const JsonRpcException& je) {
      throw je;
    } catch (const char* msg) {
      if (string(msg) == "Blocknumber Absent") {
        LOG_GENERAL(INFO, "No DSBlock has been mined yet");
      }
      return 0;
    }
  }
  uint64_t TimeDiff =
      m_mediator.m_dsBlockChain.GetLastBlock().GetTimestamp() - m_StartTimeDs;

  if (TimeDiff == 0) {
    LOG_GENERAL(INFO, "Wait till the second block");
    return 0;
  }
  // To convert from microSeconds to seconds
  numDs = numDs * 1000000;
  boost::multiprecision::cpp_dec_float_50 TimeDiffFloat =
      static_cast<boost::multiprecision::cpp_dec_float_50>(TimeDiff);
  boost::multiprecision::cpp_dec_float_50 ans = numDs / TimeDiffFloat;
  return ans.convert_to<double>();
}

double LookupServer::GetTxBlockRate() {
  LOG_MARKER();

  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }

  string numTxblockStr = to_string(m_mediator.m_txBlockChain.GetBlockCount());
  boost::multiprecision::cpp_dec_float_50 numTx(numTxblockStr);

  if (m_StartTimeTx == 0) {
    try {
      // Reference Time chosen to be first block's timestamp
      TxBlock txb = m_mediator.m_txBlockChain.GetBlock(1);
      m_StartTimeTx = txb.GetTimestamp();
    } catch (const char* msg) {
      if (string(msg) == "Blocknumber Absent") {
        LOG_GENERAL(INFO, "No TxBlock has been mined yet");
      }
      return 0;
    }
  }
  uint64_t TimeDiff =
      m_mediator.m_txBlockChain.GetLastBlock().GetTimestamp() - m_StartTimeTx;

  if (TimeDiff == 0) {
    LOG_GENERAL(INFO, "Wait till the second block");
    return 0;
  }
  // To convert from microSeconds to seconds
  numTx = numTx * 1000000;
  boost::multiprecision::cpp_dec_float_50 TimeDiffFloat(to_string(TimeDiff));
  boost::multiprecision::cpp_dec_float_50 ans = numTx / TimeDiffFloat;
  return ans.convert_to<double>();
}

Json::Value LookupServer::DSBlockListing(unsigned int page) {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  uint64_t currBlockNum =
      m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum();
  Json::Value _json;

  uint maxPages = (currBlockNum / PAGE_SIZE) + 1;

  if (currBlockNum == INIT_BLOCK_NUMBER) {
    throw JsonRpcException(RPC_IN_WARMUP, "No DS blocks");
  }

  _json["maxPages"] = maxPages;

  lock_guard<mutex> g(m_mutexDSBlockCache);

  if (m_DSBlockCache.second.size() == 0) {
    try {
      // add the hash of genesis block
      DSBlockHeader dshead = m_mediator.m_dsBlockChain.GetBlock(0).GetHeader();
      SHA2<HashType::HASH_VARIANT_256> sha2;
      bytes vec;
      dshead.Serialize(vec, 0);
      sha2.Update(vec);
      const bytes& resVec = sha2.Finalize();
      string resStr;
      DataConversion::Uint8VecToHexStr(resVec, resStr);
      m_DSBlockCache.second.insert_new(m_DSBlockCache.second.size(), resStr);
    } catch (const char* msg) {
      throw JsonRpcException(RPC_MISC_ERROR, string(msg));
    }
  }

  if (page > maxPages || page < 1) {
    throw JsonRpcException(RPC_INVALID_PARAMETER, "Pages out of limit");
  }

  if (currBlockNum > m_DSBlockCache.first) {
    for (uint64_t i = m_DSBlockCache.first + 1; i < currBlockNum; i++) {
      m_DSBlockCache.second.insert_new(m_DSBlockCache.second.size(),
                                       m_mediator.m_dsBlockChain.GetBlock(i + 1)
                                           .GetHeader()
                                           .GetPrevHash()
                                           .hex());
    }
    // for the latest block
    DSBlockHeader dshead =
        m_mediator.m_dsBlockChain.GetBlock(currBlockNum).GetHeader();
    SHA2<HashType::HASH_VARIANT_256> sha2;
    bytes vec;
    dshead.Serialize(vec, 0);
    sha2.Update(vec);
    const bytes& resVec = sha2.Finalize();
    string resStr;
    DataConversion::Uint8VecToHexStr(resVec, resStr);

    m_DSBlockCache.second.insert_new(m_DSBlockCache.second.size(), resStr);
    m_DSBlockCache.first = currBlockNum;
  }

  unsigned int offset = PAGE_SIZE * (page - 1);
  Json::Value tmpJson;
  if (page <= NUM_PAGES_CACHE)  // can use cache
  {
    uint128_t cacheSize(m_DSBlockCache.second.capacity());
    if (cacheSize > m_DSBlockCache.second.size()) {
      cacheSize = m_DSBlockCache.second.size();
    }

    uint64_t size = m_DSBlockCache.second.size();

    for (unsigned int i = offset; i < PAGE_SIZE + offset && i < cacheSize;
         i++) {
      tmpJson.clear();
      tmpJson["Hash"] = m_DSBlockCache.second[size - i - 1];
      tmpJson["BlockNum"] = uint(currBlockNum - i);
      _json["data"].append(tmpJson);
    }
  } else {
    for (uint64_t i = offset; i < PAGE_SIZE + offset && i <= currBlockNum;
         i++) {
      tmpJson.clear();
      tmpJson["Hash"] = m_mediator.m_dsBlockChain.GetBlock(currBlockNum - i + 1)
                            .GetHeader()
                            .GetPrevHash()
                            .hex();
      tmpJson["BlockNum"] = uint(currBlockNum - i);
      _json["data"].append(tmpJson);
    }
  }

  return _json;
}

Json::Value LookupServer::TxBlockListing(unsigned int page) {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  uint64_t currBlockNum =
      m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetBlockNum();
  Json::Value _json;

  if (currBlockNum == INIT_BLOCK_NUMBER) {
    throw JsonRpcException(RPC_IN_WARMUP, "No Tx blocks");
  }

  uint maxPages = (currBlockNum / PAGE_SIZE) + 1;

  _json["maxPages"] = maxPages;

  lock_guard<mutex> g(m_mutexTxBlockCache);

  if (m_TxBlockCache.second.size() == 0) {
    try {
      // add the hash of genesis block
      TxBlockHeader txhead = m_mediator.m_txBlockChain.GetBlock(0).GetHeader();
      SHA2<HashType::HASH_VARIANT_256> sha2;
      bytes vec;
      txhead.Serialize(vec, 0);
      sha2.Update(vec);
      const bytes& resVec = sha2.Finalize();
      string resStr;
      DataConversion::Uint8VecToHexStr(resVec, resStr);
      m_TxBlockCache.second.insert_new(m_TxBlockCache.second.size(), resStr);
    } catch (const char* msg) {
      throw JsonRpcException(RPC_MISC_ERROR, string(msg));
    }
  }

  if (page > maxPages || page < 1) {
    throw JsonRpcException(RPC_INVALID_PARAMETER, "Pages out of limit");
  }

  if (currBlockNum > m_TxBlockCache.first) {
    for (uint64_t i = m_TxBlockCache.first + 1; i < currBlockNum; i++) {
      m_TxBlockCache.second.insert_new(m_TxBlockCache.second.size(),
                                       m_mediator.m_txBlockChain.GetBlock(i + 1)
                                           .GetHeader()
                                           .GetPrevHash()
                                           .hex());
    }
    // for the latest block
    TxBlockHeader txhead =
        m_mediator.m_txBlockChain.GetBlock(currBlockNum).GetHeader();
    SHA2<HashType::HASH_VARIANT_256> sha2;
    bytes vec;
    txhead.Serialize(vec, 0);
    sha2.Update(vec);
    const bytes& resVec = sha2.Finalize();
    string resStr;
    DataConversion::Uint8VecToHexStr(resVec, resStr);

    m_TxBlockCache.second.insert_new(m_TxBlockCache.second.size(), resStr);
    m_TxBlockCache.first = currBlockNum;
  }

  unsigned int offset = PAGE_SIZE * (page - 1);
  Json::Value tmpJson;
  if (page <= NUM_PAGES_CACHE)  // can use cache
  {
    uint128_t cacheSize(m_TxBlockCache.second.capacity());

    if (cacheSize > m_TxBlockCache.second.size()) {
      cacheSize = m_TxBlockCache.second.size();
    }

    uint64_t size = m_TxBlockCache.second.size();

    for (unsigned int i = offset; i < PAGE_SIZE + offset && i < cacheSize;
         i++) {
      tmpJson.clear();
      tmpJson["Hash"] = m_TxBlockCache.second[size - i - 1];
      tmpJson["BlockNum"] = uint(currBlockNum - i);
      _json["data"].append(tmpJson);
    }
  } else {
    for (uint64_t i = offset; i < PAGE_SIZE + offset && i <= currBlockNum;
         i++) {
      tmpJson.clear();
      tmpJson["Hash"] = m_mediator.m_txBlockChain.GetBlock(currBlockNum - i + 1)
                            .GetHeader()
                            .GetPrevHash()
                            .hex();
      tmpJson["BlockNum"] = uint(currBlockNum - i);
      _json["data"].append(tmpJson);
    }
  }

  return _json;
}

Json::Value LookupServer::GetBlockchainInfo() {
  Json::Value _json;
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  _json["NumPeers"] = LookupServer::GetNumPeers();
  _json["NumTxBlocks"] = LookupServer::GetNumTxBlocks();
  _json["NumDSBlocks"] = LookupServer::GetNumDSBlocks();
  _json["NumTransactions"] = LookupServer::GetNumTransactions();
  _json["TransactionRate"] = LookupServer::GetTransactionRate();
  _json["TxBlockRate"] = LookupServer::GetTxBlockRate();
  _json["DSBlockRate"] = LookupServer::GetDSBlockRate();
  _json["CurrentMiniEpoch"] = LookupServer::GetCurrentMiniEpoch();
  _json["CurrentDSEpoch"] = LookupServer::GetCurrentDSEpoch();
  _json["NumTxnsDSEpoch"] = LookupServer::GetNumTxnsDSEpoch();
  _json["NumTxnsTxEpoch"] = LookupServer::GetNumTxnsTxEpoch();
  _json["ShardingStructure"] = LookupServer::GetShardingStructure();

  return _json;
}

Json::Value LookupServer::GetRecentTransactions() {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  lock_guard<mutex> g(m_mutexRecentTxns);
  Json::Value _json;
  uint64_t actualSize(m_RecentTransactions.capacity());
  if (actualSize > m_RecentTransactions.size()) {
    actualSize = m_RecentTransactions.size();
  }
  uint64_t size = m_RecentTransactions.size();
  _json["number"] = uint(actualSize);
  _json["TxnHashes"] = Json::Value(Json::arrayValue);
  for (uint64_t i = 0; i < actualSize; i++) {
    _json["TxnHashes"].append(m_RecentTransactions[size - i - 1]);
  }

  return _json;
}

void LookupServer::AddToRecentTransactions(const TxnHash& txhash) {
  lock_guard<mutex> g(m_mutexRecentTxns);
  m_RecentTransactions.insert_new(m_RecentTransactions.size(), txhash.hex());
}
Json::Value LookupServer::GetShardingStructure() {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  try {
    Json::Value _json;

    auto shards = m_mediator.m_lookup->GetShardPeers();

    unsigned int num_shards = shards.size();

    if (num_shards == 0) {
      throw JsonRpcException(RPC_IN_WARMUP, "No shards yet");
    } else {
      for (unsigned int i = 0; i < num_shards; i++) {
        _json["NumPeers"].append(static_cast<unsigned int>(shards[i].size()));
      }
    }
    return _json;

  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(WARNING, e.what());
    throw JsonRpcException(RPC_MISC_ERROR, "Unable to process");
  }
}

string LookupServer::GetNumTxnsTxEpoch() {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  try {
    return to_string(
        m_mediator.m_txBlockChain.GetLastBlock().GetHeader().GetNumTxs());
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (exception& e) {
    LOG_GENERAL(WARNING, e.what());
    return "0";
  }
}

string LookupServer::GetNumTxnsDSEpoch() {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  try {
    auto latestTxBlock = m_mediator.m_txBlockChain.GetLastBlock().GetHeader();
    auto latestTxBlockNum = latestTxBlock.GetBlockNum();
    auto latestDSBlockNum = latestTxBlock.GetDSBlockNum();

    lock_guard<mutex> g(m_mutexTxBlockCountSumPair);

    if (latestTxBlockNum > m_TxBlockCountSumPair.first) {
      // Case where the DS Epoch is same
      if (m_mediator.m_txBlockChain.GetBlock(m_TxBlockCountSumPair.first)
              .GetHeader()
              .GetDSBlockNum() == latestDSBlockNum) {
        for (auto i = latestTxBlockNum; i > m_TxBlockCountSumPair.first; i--) {
          m_TxBlockCountSumPair.second +=
              m_mediator.m_txBlockChain.GetBlock(i).GetHeader().GetNumTxs();
        }
      }
      // Case if DS Epoch Changed
      else {
        m_TxBlockCountSumPair.second = 0;

        for (auto i = latestTxBlockNum; i > m_TxBlockCountSumPair.first; i--) {
          if (m_mediator.m_txBlockChain.GetBlock(i)
                  .GetHeader()
                  .GetDSBlockNum() < latestDSBlockNum) {
            break;
          }
          m_TxBlockCountSumPair.second +=
              m_mediator.m_txBlockChain.GetBlock(i).GetHeader().GetNumTxs();
        }
      }

      m_TxBlockCountSumPair.first = latestTxBlockNum;
    }

    return m_TxBlockCountSumPair.second.str();
  } catch (const JsonRpcException& je) {
    throw je;
  }

  catch (exception& e) {
    LOG_GENERAL(WARNING, e.what());
    return "0";
  }
}

Json::Value LookupServer::GetTransactionsForTxBlock(const string& txBlockNum) {
  LOG_MARKER();
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  uint64_t txNum;
  Json::Value _json = Json::arrayValue;
  try {
    txNum = strtoull(txBlockNum.c_str(), NULL, 0);
  } catch (exception& e) {
    throw JsonRpcException(RPC_INVALID_PARAMETER, e.what());
  }

  auto const& txBlock = m_mediator.m_txBlockChain.GetBlock(txNum);

  // TODO
  // Workaround to identify dummy block as == comparator does not work on
  // empty object for TxBlock and TxBlockheader().
  // if (txBlock == TxBlock()) {
  if (txBlock.GetHeader().GetBlockNum() == INIT_BLOCK_NUMBER &&
      txBlock.GetHeader().GetDSBlockNum() == INIT_BLOCK_NUMBER) {
    throw JsonRpcException(RPC_INVALID_PARAMS, "Tx Block does not exist");
  }

  auto microBlockInfos = txBlock.GetMicroBlockInfos();

  bool hasTransactions = false;
  for (auto const& mbInfo : microBlockInfos) {
    MicroBlockSharedPtr mbptr;
    _json[mbInfo.m_shardId] = Json::arrayValue;

    if (mbInfo.m_txnRootHash == TxnHash()) {
      continue;
    }

    if (!BlockStorage::GetBlockStorage().GetMicroBlock(mbInfo.m_microBlockHash,
                                                       mbptr)) {
      if (!m_mediator.m_lookup->m_historicalDB) {
        throw JsonRpcException(RPC_DATABASE_ERROR, "Failed to get Microblock");
      } else if (!BlockStorage::GetBlockStorage().GetHistoricalMicroBlock(
                     mbInfo.m_microBlockHash, mbptr)) {
        throw JsonRpcException(RPC_DATABASE_ERROR, "Failed to get Microblock");
      }
    }

    const std::vector<TxnHash>& tranHashes = mbptr->GetTranHashes();
    if (tranHashes.size() > 0) {
      hasTransactions = true;
      for (const auto& tranHash : tranHashes) {
        _json[mbInfo.m_shardId].append(tranHash.hex());
      }
    }
  }

  if (!hasTransactions) {
    throw JsonRpcException(RPC_MISC_ERROR, "TxBlock has no transactions");
  }

  return _json;
}

vector<uint> GenUniqueIndices(uint32_t size, uint32_t num, mt19937& eng) {
  // case when the number required is greater than total numbers being shuffled
  if (size < num) {
    num = size;
  }
  if (num == 0) {
    return vector<uint>();
  }
  vector<uint> v(num);

  for (uint i = 0; i < num; i++) {
    uniform_int_distribution<> dis(
        0, size - i - 1);  // random num between 0 to i-1
    uint x = dis(eng);
    uint j = 0;
    for (j = 0; j < i; j++) {
      if (x < v.at(j)) {
        break;
      }
      x++;
    }
    for (uint k = j + 1; k <= i; k++) {
      v.at(i + j + 1 - k) = v.at(i + j - k);
    }
    v.at(j) = x;
  }
  return v;
}

Json::Value LookupServer::GetShardMembers(unsigned int shardID) {
  if (!LOOKUP_NODE_MODE) {
    throw JsonRpcException(RPC_INVALID_REQUEST, "Sent to a non-lookup");
  }
  const auto shards = m_mediator.m_lookup->GetShardPeers();
  const auto& num_shards = shards.size();
  if (num_shards <= shardID) {
    throw JsonRpcException(RPC_INVALID_PARAMETER, "Invalid shard ID");
  }
  Json::Value _json;
  try {
    const auto& shard = shards.at(shardID);
    if (shard.empty()) {
      throw JsonRpcException(RPC_INVALID_PARAMETER, "Shard size 0");
    }

    auto random_vec =
        GenUniqueIndices(shard.size(), NUM_SHARD_PEER_TO_REVEAL, m_eng);
    for (auto const& x : random_vec) {
      const auto& node = shard.at(x);
      _json.append(JSONConversion::convertNode(node));
    }
    return _json;
  } catch (const JsonRpcException& je) {
    throw je;
  } catch (const exception& e) {
    LOG_GENERAL(WARNING, "[Error] " << e.what());
    throw JsonRpcException(RPC_MISC_ERROR, "Unable to process");
  }
}
