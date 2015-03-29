
/*
 * @copyright
 *
 *  Copyright 2013 Neo Natura
 *
 *  This file is part of the Share Library.
 *  (https://github.com/neonatura/share)
 *        
 *  The Share Library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version. 
 *
 *  The Share Library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with The Share Library.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  @endcopyright
 */

#include "db.h"
#include "walletdb.h"
#include "server/rpc_proto.h"
#include "net.h"
#include "init.h"
#include "util.h"
#include "ui_interface.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/convenience.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/algorithm/string/predicate.hpp>

#ifndef WIN32
#include <signal.h>
#endif

using namespace std;
using namespace boost;
using namespace json_spirit;

extern CWallet* pwalletMain;

extern Value ValueFromAmount(int64 amount);

extern int64 GetAccountBalance(CWalletDB& walletdb, const string& strAccount, int nMinDepth);

extern void AcentryToJSON(const CAccountingEntry& acentry, const string& strAccount, Array& ret);

extern string JSONRPCReply(const Value& result, const Value& error, const Value& id);

extern void ListTransactions(const CWalletTx& wtx, const string& strAccount, int nMinDepth, bool fLong, Array& ret);

class DescribeAddressVisitor : public boost::static_visitor<Object>
{
public:
    Object operator()(const CNoDestination &dest) const { return Object(); }

    Object operator()(const CKeyID &keyID) const {
        Object obj;
        CPubKey vchPubKey;
        pwalletMain->GetPubKey(keyID, vchPubKey);
        obj.push_back(Pair("isscript", false));
        obj.push_back(Pair("pubkey", HexStr(vchPubKey.Raw())));
        obj.push_back(Pair("iscompressed", vchPubKey.IsCompressed()));
        return obj;
    }

    Object operator()(const CScriptID &scriptID) const {
        Object obj;
        obj.push_back(Pair("isscript", true));
        CScript subscript;
        pwalletMain->GetCScript(scriptID, subscript);
        std::vector<CTxDestination> addresses;
        txnouttype whichType;
        int nRequired;
        ExtractDestinations(subscript, whichType, addresses, nRequired);
        obj.push_back(Pair("script", GetTxnOutputType(whichType)));
        Array a;
        BOOST_FOREACH(const CTxDestination& addr, addresses)
            a.push_back(CBitcoinAddress(addr).ToString());
        obj.push_back(Pair("addresses", a));
        if (whichType == TX_MULTISIG)
            obj.push_back(Pair("sigsrequired", nRequired));
        return obj;
    }
};


string address;

Object stratumerror_obj;
void SetStratumError(Object error)
{
  stratumerror_obj = error;
}
Object GetStratumError(void)
{
  return (stratumerror_obj);
}

int c_LoadWallet(void)
{
    int64 nStart;
    std::ostringstream strErrors;

    const char* pszP2SH = "/P2SH/";
    COINBASE_FLAGS << std::vector<unsigned char>(pszP2SH, pszP2SH+strlen(pszP2SH));

    if (!bitdb.Open(GetDataDir()))
    {
fprintf(stderr, "error: unable to open data directory.\n");
        return (-1);
    }

    if (!LoadBlockIndex()) {
fprintf(stderr, "error: unable to open load block index.\n");
      return (-1);
    }

    nStart = GetTimeMillis();
    bool fFirstRun = true;
    pwalletMain = new CWallet("wallet.dat");
    pwalletMain->LoadWallet(fFirstRun);

    if (fFirstRun)
    {

        // Create new keyUser and set as default key
        RandAddSeedPerfmon();

        CPubKey newDefaultKey;
        if (!pwalletMain->GetKeyFromPool(newDefaultKey, false))
            strErrors << _("Cannot initialize keypool") << "\n";
        pwalletMain->SetDefaultKey(newDefaultKey);
        if (!pwalletMain->SetAddressBookName(pwalletMain->vchDefaultKey.GetID(), ""))
            strErrors << _("Cannot write default address") << "\n";
    }

    printf("%s", strErrors.str().c_str());

    RegisterWallet(pwalletMain);

    CBlockIndex *pindexRescan = pindexBest;
    if (GetBoolArg("-rescan"))
        pindexRescan = pindexGenesisBlock;
    else
    {
        CWalletDB walletdb("wallet.dat");
        CBlockLocator locator;
        if (walletdb.ReadBestBlock(locator))
            pindexRescan = locator.GetBlockIndex();
    }
    if (pindexBest != pindexRescan && pindexBest && pindexRescan && pindexBest->nHeight > pindexRescan->nHeight)
    {
        printf("Rescanning last %i blocks (from block %i)...\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);
        nStart = GetTimeMillis();
        pwalletMain->ScanForWalletTransactions(pindexRescan, true);
        printf(" rescan      %15"PRI64d"ms\n", GetTimeMillis() - nStart);
    }


    /** load peers */
    printf("Loading addresses...\n");
    nStart = GetTimeMillis();

    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            printf("Invalid or missing peers.dat; recreating\n");
    }

    printf("Loaded %i addresses from peers.dat  %"PRI64d"ms\n",
           addrman.size(), GetTimeMillis() - nStart);

    RandAddSeedPerfmon();
    pwalletMain->ReacceptWalletTransactions();
}

CBitcoinAddress GetNewAddress(string strAccount)
{
  if (!pwalletMain->IsLocked())
    pwalletMain->TopUpKeyPool();

  // Generate a new key that is added to wallet
  CPubKey newKey;
  if (!pwalletMain->GetKeyFromPool(newKey, false)) {
    throw JSONRPCError(-12, "Error: Keypool ran out, please call keypoolrefill first");
  }
  CKeyID keyID = newKey.GetID();

  pwalletMain->SetAddressBookName(keyID, strAccount);

  return CBitcoinAddress(keyID);
}
string c_getnewaddress(string strAccount)
{
  return (GetNewAddress(strAccount).ToString());
/*
  if (!pwalletMain->IsLocked())
    pwalletMain->TopUpKeyPool();

  // Generate a new key that is added to wallet
  CPubKey newKey;
  if (!pwalletMain->GetKeyFromPool(newKey, false))
    throw JSONRPCError(-12, "Error: Keypool ran out, please call keypoolrefill first");
  CKeyID keyID = newKey.GetID();

  pwalletMain->SetAddressBookName(keyID, strAccount);

  return CBitcoinAddress(keyID).ToString();
*/
}


CBitcoinAddress GetAddressByAccount(const char *accountName)
{
  CBitcoinAddress address;
  string strAccount(accountName);
  Array ret;
  bool found = false;

  // Find all addresses that have the given account
  BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
  {
    const CBitcoinAddress& acc_address = item.first;
    const string& strName = item.second;
    if (strName == strAccount) {
      address = acc_address;
    }
  }

  return (address);
}

const char *c_getaddressbyaccount(const char *accountName)
{
  address = GetAddressByAccount(accountName).ToString();
  return (address.c_str());
/*
  string strAccount(accountName);
  Array ret;
  bool found = false;

  // Find all addresses that have the given account
  BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
  {
    const CBitcoinAddress& acc_address = item.first;
    const string& strName = item.second;
    if (strName == strAccount) {
      address = acc_address.ToString();
      found = true;
    }
  }

  if (!found)
    address = c_getnewaddress(strAccount);

  return (address.c_str());
*/
}

/**
 * Sends a reward to a particular address.
 */
int c_setblockreward(const char *accountName, double dAmount)
{
  CWalletDB walletdb(pwalletMain->strWalletFile);
  string strMainAccount("");
  string strAccount(accountName);
  string strComment("sharenet");
  int64 nAmount;
  Array ret;
  int nMinDepth = 1; /* single confirmation requirement */
  int nMinConfirmDepth = 1; /* single confirmation requirement */
  bool found = false;
  int64 nBalance;

  if (pwalletMain->IsLocked()) {
fprintf(stderr, "DEBUG: wallet is locked.\n");
    return (-13);
}

  const CBitcoinAddress address = GetAddressByAccount(accountName);



  if (dAmount <= 0.0 || dAmount > 84000000.0) {
fprintf(stderr, "DEBUG: invalid amount (%f)\n", dAmount);
    //throw JSONRPCError(-3, "Invalid amount");
    return (-3);
  }

  nAmount = roundint64(dAmount * COIN);
  if (!MoneyRange(nAmount)) {
fprintf(stderr, "DEBUG: invalid amount: !MoneyRange(%d)\n", (int)nAmount);
    //throw JSONRPCError(-3, "Invalid amount");
    return (-3);
  }


  nBalance  = GetAccountBalance(walletdb, strMainAccount, nMinConfirmDepth);
  if (nAmount > nBalance) {
fprintf(stderr, "DEBUG: account has insufficient funds\n");
    //throw JSONRPCError(-6, "Account has insufficient funds");
    return (-6);
  }

  //address = GetAddressByAccount(accountName);
  if (!address.IsValid()) {
fprintf(stderr, "DEBUG: invalid usde address destination\n");
    //throw JSONRPCError(-5, "Invalid usde address");
    return (-5);
  }

/*
  BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
  {
    const CBitcoinAddress& acc_address = item.first;
    const string& strName = item.second;
    if (strName == strAccount) {
      address = acc_address;
      found = true;
    }
  }

  if (!found) {
    if (!pwalletMain->IsLocked())
      pwalletMain->TopUpKeyPool();

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwalletMain->GetKeyFromPool(newKey, false)) {
      //throw JSONRPCError(-12, "Error: Keypool ran out, please call keypoolrefill first");
      return (-12);
    }
    CKeyID keyID = newKey.GetID();
    pwalletMain->SetAddressBookName(keyID, strAccount);
    address = CBitcoinAddress(keyID);
  }
  
*/

  CWalletTx wtx;
  wtx.strFromAccount = strMainAccount;
  wtx.mapValue["comment"] = strComment;
  string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
  if (strError != "") {
fprintf(stderr, "DEBUG: '%s' = SendMoneyTo: amount %d\n", strError.c_str(), (int)nAmount);
    //throw JSONRPCError(-4, strError);
    return (-4);
  }

fprintf(stderr, "DEBUG: c_set_block_reward: reward (%s -> %f).\n", accountName, dAmount);


  return (0);
}

/**
 * Transfer currency between two accounts.
 */
static int c_wallet_account_transfer(const char *sourceAccountName,
    const char *accountName, const char *comment, double dAmount)
{

  if (0 == strcmp(sourceAccountName, ""))
    return (-14);

  CWalletDB walletdb(pwalletMain->strWalletFile);
  CBitcoinAddress address;
  string strMainAccount(sourceAccountName);
  string strAccount(accountName);
  string strComment(comment);
  int64 nAmount;
  Array ret;
  int nMinDepth = 1; /* single confirmation requirement */
  int nMinConfirmDepth = 1; /* single confirmation requirement */
  bool found = false;
  int64 nBalance;

  if (pwalletMain->IsLocked()) {
    fprintf(stderr, "DEBUG: wallet is locked.\n");
    return (-13);
  }

  // Find all addresses that have the given account
  BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
  {
    const CBitcoinAddress& acc_address = item.first;
    const string& strName = item.second;
    if (strName == strAccount) {
      address = acc_address;
      found = true;
    }
  }
  if (!found) {
    return (-7);
  }

  if (dAmount <= 0.0 || dAmount > 84000000.0) {
    fprintf(stderr, "DEBUG: invalid amount (%f)\n", dAmount);
    //throw JSONRPCError(-3, "Invalid amount");
    return (-3);
  }

  nAmount = roundint64(dAmount * COIN);
  if (!MoneyRange(nAmount)) {
    fprintf(stderr, "DEBUG: invalid amount: !MoneyRange(%d)\n", (int)nAmount);
    //throw JSONRPCError(-3, "Invalid amount");
    return (-3);
  }


  nBalance  = GetAccountBalance(walletdb, strMainAccount, nMinConfirmDepth);
  if (nAmount > nBalance) {
    fprintf(stderr, "DEBUG: account has insufficient funds\n");
    //throw JSONRPCError(-6, "Account has insufficient funds");
    return (-6);
  }

  //address = GetAddressByAccount(accountName);
  if (!address.IsValid()) {
    fprintf(stderr, "DEBUG: invalid usde address destination\n");
    //throw JSONRPCError(-5, "Invalid usde address");
    return (-5);
  }

  CWalletTx wtx;
  wtx.strFromAccount = strMainAccount;
  wtx.mapValue["comment"] = strComment;
  string strError = pwalletMain->SendMoneyToDestination(address.Get(), nAmount, wtx);
  if (strError != "") {
    fprintf(stderr, "DEBUG: '%s' = SendMoneyTo: amount %d\n", strError.c_str(), (int)nAmount);
    return (-4);
  }

  return (0);
}

double c_getaccountbalance(const char *accountName)
{
  CWalletDB walletdb(pwalletMain->strWalletFile);
  string strAccount(accountName);

  int nMinDepth = 1;
  int64 nBalance = GetAccountBalance(walletdb, strAccount, nMinDepth);

  return ((double)nBalance / (double)COIN);
}

static uint256 get_private_key_hash(CKeyID keyId)
{
  CSecret vchSecret;
  bool fCompressed;
  if (!pwalletMain->GetSecret(keyId, vchSecret, fCompressed))
    return (NULL);
  string secret = CBitcoinSecret(vchSecret, fCompressed).ToString();

  uint256 phash;
  unsigned char *secret_str = (unsigned char *)secret.c_str();
  size_t secret_len = secret.length();
  SHA256(secret_str, secret_len, (unsigned char*)&phash);

  return (phash);
}

int valid_pkey_hash(string strAccount, uint256 in_pkey)
{
  uint256 acc_pkey;
  int valid;

  valid = 0;
  acc_pkey = 0;
  BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
  {
    const CBitcoinAddress& address = item.first;
    const string& strName = item.second;
    CKeyID keyID;

    if (strName != strAccount)
      continue;
    if (!address.GetKeyID(keyID))
      continue;

    acc_pkey = get_private_key_hash(keyID);
    if (acc_pkey == in_pkey)
      valid++;
else fprintf(stderr, "DEBUG: get_private_key_hash: '%s'\n", acc_pkey.GetHex().c_str());
  }

  return (valid);
}

/**
 * local known transactions associated with account name.
 * @returns json string format 
 */
string accounttransactioninfo_json;
static const char *cxx_getaccounttransactioninfo(const char *tx_account, const char *pkey_str, int duration)
{
  string strAccount(tx_account);
  uint256 in_pkey = 0;
  Array result;
  int64 min_t;

  try {
    in_pkey.SetHex(pkey_str);
    if (!valid_pkey_hash(strAccount, in_pkey)) {
      throw JSONRPCError(STERR_ACCESS, "Invalid private key hash specified.");
    }

    min_t = time(NULL) - duration;
    CWalletDB walletdb(pwalletMain->strWalletFile);
    for (map<uint256, CWalletTx>::iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
      CWalletTx* wtx = &((*it).second);

      if (wtx->GetTxTime() >= min_t)
        ListTransactions(*wtx, strAccount, 0, true, result);
    }
  } catch(Object& objError) {
    SetStratumError(objError);
    return (NULL);
  }

  accounttransactioninfo_json = JSONRPCReply(result, Value::null, Value::null);
  return (accounttransactioninfo_json.c_str());
}

string addressinfo_json;
const char *c_getaddressinfo(const char *addr_hash)
{
  string strAddr(addr_hash);
  Object result;

  try {
    CBitcoinAddress address(strAddr);
    if (!address.IsValid()) {
      throw JSONRPCError(STERR_INVAL, "Invalid usde destination address");
    }

    CTxDestination dest = address.Get();
    string currentAddress = address.ToString();
    result.push_back(Pair("address", currentAddress));
    bool fMine = IsMine(*pwalletMain, dest);

    Object detail = boost::apply_visitor(DescribeAddressVisitor(), dest);
    result.insert(result.end(), detail.begin(), detail.end());
    if (pwalletMain->mapAddressBook.count(dest))
      result.push_back(Pair("account", pwalletMain->mapAddressBook[dest]));
  } catch(Object& objError) {
    SetStratumError(objError);
    return (NULL);
  }

  addressinfo_json = JSONRPCReply(result, Value::null, Value::null);
  return (addressinfo_json.c_str());
}

string createaccount_json;
static const char *c_stratum_create_account(const char *acc_name)
{
  string strAccount(acc_name);
  string coinAddr = "";
  uint256 phash = 0;
  CPubKey newKey;

  try {
    if (strAccount == "" || strAccount == "*") {
      throw JSONRPCError(STERR_INVAL_PARAM, "The account name specified is invalid.");
    }

    CBitcoinAddress address = GetAddressByAccount(acc_name);
    if (address.IsValid()) {
      throw JSONRPCError(STERR_INVAL_PARAM, "Account name is not unique.");
    }

    /* Generate a new key that is added to wallet. */
    if (!pwalletMain->GetKeyFromPool(newKey, false)) {
      if (!pwalletMain->IsLocked())
        pwalletMain->TopUpKeyPool();
      if (!pwalletMain->GetKeyFromPool(newKey, false)) {
        throw JSONRPCError(STERR_INTERNAL_MAP, "No new keys currently available.");
        return (NULL);
      }
    }

    CKeyID keyId = newKey.GetID();
    pwalletMain->SetAddressBookName(keyId, strAccount);
    coinAddr = CBitcoinAddress(keyId).ToString();
    phash = get_private_key_hash(keyId);
  } catch(Object& objError) {
    SetStratumError(objError);
    return (NULL);
  }

  Object result;
  result.push_back(Pair("address", coinAddr));
  result.push_back(Pair("key", phash.GetHex()));
  createaccount_json = JSONRPCReply(result, Value::null, Value::null);
  return (createaccount_json.c_str());
}

string transferaccount_json;
static const char *c_stratum_account_transfer(char *account, char *pkey_str, char *dest, double amount)
{
  CWalletDB walletdb(pwalletMain->strWalletFile);
  string strAccount(account);
  string strDestAddress(dest);
  CBitcoinAddress dest_address(strDestAddress);
  CWalletTx wtx;
  int64 nAmount;
  string strAddress;
  CKeyID keyID;
  CSecret vchSecret;
  bool fCompressed;
  uint256 acc_pkey;
  uint256 in_pkey;
  int nMinDepth;
  int64 nBalance;

  try {
    in_pkey = 0;
    nMinDepth = 1;
    nAmount = roundint64(amount * COIN);

    if (pwalletMain->IsLocked()) {
      throw JSONRPCError(STERR_ACCESS_NOKEY, "Account transactions are not currently available.");
    }

    if (!dest_address.IsValid()) {
      throw JSONRPCError(STERR_INVAL, "Invalid usde destination address");
    }

    in_pkey.SetHex(pkey_str);
    if (!valid_pkey_hash(strAccount, in_pkey)) {
      throw JSONRPCError(STERR_ACCESS, "Invalid private key hash specified.");
    }

    nBalance = GetAccountBalance(walletdb, strAccount, nMinDepth);
    if (nAmount > nBalance) {
      throw JSONRPCError(STERR_FUND_UNAVAIL, "Account has insufficient funds.");
    }

    wtx.strFromAccount = strAccount;
    wtx.mapValue["comment"] = "sharelib.net";
    string strError = pwalletMain->SendMoneyToDestination(dest_address.Get(), nAmount, wtx);
    if (strError != "") {
      throw JSONRPCError(STERR_ACCESS_UNAVAIL, strError);
    }
  } catch(Object& objError) {
    SetStratumError(objError);
    return (NULL);
  }

  Object result;
  result.push_back(Pair("txid", wtx.GetHash().GetHex()));
  transferaccount_json = JSONRPCReply(result, Value::null, Value::null);
  return (transferaccount_json.c_str());
}

string accountinfo_json;
static const char *c_stratum_account_info(const char *acc_name, const char *pkey_str)
{
  CWalletDB walletdb(pwalletMain->strWalletFile);
  string strAccount(acc_name);
  int64 nConfirm;
  int64 nUnconfirm;
  int nMinDepth = 1;
  uint256 in_pkey;
  Object result;
  Array addr_list;
  CBitcoinAddress address;
  uint256 phash;

  try {
    if (strAccount == "" || strAccount == "*") {
      throw JSONRPCError(STERR_INVAL_PARAM, "The account name specified is invalid.");
    }

    in_pkey.SetHex(pkey_str);
    if (!valid_pkey_hash(strAccount, in_pkey)) {
      throw JSONRPCError(STERR_ACCESS, "Invalid private key hash specified for account.");
    }

    nConfirm = GetAccountBalance(walletdb, strAccount, nMinDepth);
    nUnconfirm = GetAccountBalance(walletdb, strAccount, 0) - nConfirm;
    result.push_back(Pair("confirmed", ValueFromAmount(nConfirm)));
    result.push_back(Pair("unconfirmed", ValueFromAmount(nUnconfirm)));

    // Find all addresses that have the given account
    BOOST_FOREACH(const PAIRTYPE(CBitcoinAddress, string)& item, pwalletMain->mapAddressBook)
    {
      const CBitcoinAddress& acc_address = item.first;
      const string& strName = item.second;
      if (strName == strAccount) {
        addr_list.push_back(acc_address.ToString());

        CKeyID keyID;
        acc_address.GetKeyID(keyID);
        phash = get_private_key_hash(keyID);
      }
    }
    result.push_back(Pair("addresses", addr_list));
  } catch(Object& objError) {
    SetStratumError(objError);
    return (NULL);
  }

  accountinfo_json = JSONRPCReply(result, Value::null, Value::null);
  return (accountinfo_json.c_str());
}


string stratumerror_json;
const char *c_stratum_error_get(int req_id)
{
  Object error;
  Object reply;
  Value id = req_id;

  error = GetStratumError();
  stratumerror_json = JSONRPCReply(Value::null, error, id);
  return (stratumerror_json.c_str());
}

#ifdef __cplusplus
extern "C" {
#endif

int load_wallet(void)
{
  return (c_LoadWallet());
}

const char *getaddressbyaccount(const char *accountName)
{
  if (!*accountName)
    return ("");
  return (c_getaddressbyaccount(accountName));
}

double getaccountbalance(const char *accountName)
{
  return (c_getaccountbalance(accountName));
}

int setblockreward(const char *accountName, double amount)
{
  if (!*accountName)
    return (-5); /* invalid usde address */
  return (c_setblockreward(accountName, amount));
}

int wallet_account_transfer(const char *sourceAccountName, const char *accountName, const char *comment, double amount)
{
  if (!accountName || !*accountName)
    return (-5); /* invalid usde address */
  return (c_wallet_account_transfer(sourceAccountName, accountName, comment, amount));
}

const char *getaccounttransactioninfo(const char *account, const char *pkey_str, int duration)
{
  if (!account)
    return (NULL);
  return (cxx_getaccounttransactioninfo(account, pkey_str, duration));
}

const char *getaddressinfo(const char *addr_hash)
{
  if (!addr_hash)
    return (NULL);
  return (c_getaddressinfo(addr_hash));
}

const char *stratum_create_account(const char *acc_name)
{
  if (!acc_name)
    return (NULL);
  return (c_stratum_create_account(acc_name));
}

const char *stratum_create_transaction(char *account, char *pkey_str, char *dest, double amount)
{
  if (!account || !pkey_str || !dest)
    return (NULL);
  return (c_stratum_account_transfer(account, pkey_str, dest, amount));
}

const char *stratum_getaccountinfo(const char *account, const char *pkey_str)
{
  if (!account || !pkey_str)
    return (NULL);
  return (c_stratum_account_info(account, pkey_str));
}

const char *stratum_error_get(int req_id)
{
  return (c_stratum_error_get(req_id));
}

#ifdef __cplusplus
}
#endif


