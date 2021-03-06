
/*
 * @copyright
 *
 *  Copyright 2014 Neo Natura
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



#ifndef __SHC_WALLET_H__
#define __SHC_WALLET_H__

/**
 * @ingroup sharecoin_shc
 * @{
 */


extern CScript SHC_COINBASE_FLAGS;

class SHCWallet : public CWallet
{
  public:
    SHCWallet() : CWallet(SHC_COIN_IFACE, "shc_wallet.dat") { };  

    void RelayWalletTransaction(CWalletTx& wtx);
    void ResendWalletTransactions();
    void ReacceptWalletTransactions();
    int ScanForWalletTransactions(CBlockIndex* pindexStart, bool fUpdate = false);
    int64 GetTxFee(CTransaction tx);
    bool CommitTransaction(CWalletTx& wtxNew);

    bool CreateTransaction(const std::vector<std::pair<CScript, int64> >& vecSend, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet);
    bool CreateTransaction(CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew, CReserveKey& reservekey, int64& nFeeRet);
    void AddSupportingTransactions(CWalletTx& wtx);

    bool CreateAccountTransaction(string strFromAccount, const vector<pair<CScript, int64> >& vecSend, CWalletTx& wtxNew, string& strError, int64& nFeeRet);
    bool CreateAccountTransaction(string strFromAccount, CScript scriptPubKey, int64 nValue, CWalletTx& wtxNew, string& strError, int64& nFeeRet);


    bool UnacceptWalletTransaction(const CTransaction& tx);

    int64 GetBlockValue(int nHeight, int64 nFees);

    unsigned int GetTransactionWeight(const CTransaction& tx);

    unsigned int GetVirtualTransactionSize(int64 nWeight, int64 nSigOpCost = 0);

    unsigned int GetVirtualTransactionSize(const CTransaction& tx);

    double AllowFreeThreshold();

    int64 GetFeeRate();
};


extern SHCWallet *shcWallet;


bool shc_LoadWallet(void);


/**
 * @}
 */


#endif /* ndef __SHC_WALLET_H__ */
