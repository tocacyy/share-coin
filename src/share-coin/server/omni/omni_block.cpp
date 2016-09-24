
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

#include "shcoind.h"
#include "net.h"
#include "init.h"
#include "strlcpy.h"
#include "ui_interface.h"
#include "omni_block.h"
#include "omni_txidx.h"
#include "omni_wallet.h"
#include "chain.h"

#ifdef WIN32
#include <string.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef fcntl
#undef fcntl
#endif

#include <boost/array.hpp>
#include <boost/assign/list_of.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <share.h>


using namespace std;
using namespace boost;


uint256 omni_hashGenesisBlock("0x721abe3814e15f1ab50514c8b7fffa7578c1f35aa915275ee91f4cb8d02be5c4");
static CBigNum omni_bnStartDifficulty(~uint256(0) >> 26);
static CBigNum omni_bnProofOfWorkLimit(~uint256(0) >> 20);
static const int64 omni_nTargetTimespan = 4 * 60 * 60;
static const int64 omni_nTargetSpacing = 3 * 60;


map<uint256, OMNIBlock*> OMNI_mapOrphanBlocks;
multimap<uint256, OMNIBlock*> OMNI_mapOrphanBlocksByPrev;
map<uint256, map<uint256, CDataStream*> > OMNI_mapOrphanTransactionsByPrev;
map<uint256, CDataStream*> OMNI_mapOrphanTransactions;



class OMNIOrphan
{
  public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;

    OMNIOrphan(CTransaction* ptxIn)
    {
      ptx = ptxIn;
      dPriority = 0;
    }

    void print() const
    {
      printf("OMNIOrphan(hash=%s, dPriority=%.1f)\n", ptx->GetHash().ToString().substr(0,10).c_str(), dPriority);
      BOOST_FOREACH(uint256 hash, setDependsOn)
        printf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};


unsigned int static KimotoGravityWell(const CBlockIndex* pindexLast, const CBlockHeader *pblock, uint64 TargetBlocksSpacingSeconds, uint64 PastBlocksMin, uint64 PastBlocksMax) 
{
	const CBlockIndex  *BlockLastSolved				= pindexLast;
	const CBlockIndex  *BlockReading				= pindexLast;
	const CBlockHeader *BlockCreating				= pblock;
						BlockCreating				= BlockCreating;
	uint64				PastBlocksMass				= 0;
	int64				PastRateActualSeconds		= 0;
	int64				PastRateTargetSeconds		= 0;
	double				PastRateAdjustmentRatio		= double(1);
	CBigNum				PastDifficultyAverage;
	CBigNum				PastDifficultyAveragePrev;
	double				EventHorizonDeviation;
	double				EventHorizonDeviationFast;
	double				EventHorizonDeviationSlow;
	
    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64)BlockLastSolved->nHeight < PastBlocksMin) { return omni_bnProofOfWorkLimit.GetCompact(); }
	
	for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
		if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
		PastBlocksMass++;
		
		if (i == 1)	{ PastDifficultyAverage.SetCompact(BlockReading->nBits); }
		else		{ PastDifficultyAverage = ((CBigNum().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev; }
		PastDifficultyAveragePrev = PastDifficultyAverage;
		
		PastRateActualSeconds			= BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
		PastRateTargetSeconds			= TargetBlocksSpacingSeconds * PastBlocksMass;
		PastRateAdjustmentRatio			= double(1);
		if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
		if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
		PastRateAdjustmentRatio			= double(PastRateTargetSeconds) / double(PastRateActualSeconds);
		}
		EventHorizonDeviation			= 1 + (0.7084 * pow((double(PastBlocksMass)/double(28.2)), -1.228));
		EventHorizonDeviationFast		= EventHorizonDeviation;
		EventHorizonDeviationSlow		= 1 / EventHorizonDeviation;
		
		if (PastBlocksMass >= PastBlocksMin) {
			if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
		}
		if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
		BlockReading = BlockReading->pprev;
	}
	
	CBigNum bnNew(PastDifficultyAverage);
	if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
		bnNew *= PastRateActualSeconds;
		bnNew /= PastRateTargetSeconds;
	}
  if (bnNew > omni_bnProofOfWorkLimit) { bnNew = omni_bnProofOfWorkLimit; }

	return bnNew.GetCompact();
}



unsigned int OMNIBlock::GetNextWorkRequired(const CBlockIndex* pindexLast)
{
  if (pindexLast->nHeight == 160)
    return omni_bnStartDifficulty.GetCompact();

  static const int64  BlocksTargetSpacing     = 3 * 60;
  unsigned int    TimeDaySeconds        = 60 * 60 * 24;
  int64       PastSecondsMin        = TimeDaySeconds * 3 * 0.1;
  int64       PastSecondsMax        = TimeDaySeconds * 3 * 2.8;
  uint64        PastBlocksMin       = PastSecondsMin / BlocksTargetSpacing;
  uint64        PastBlocksMax       = PastSecondsMax / BlocksTargetSpacing;

  return KimotoGravityWell(pindexLast, this, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax);
}



int64 omni_GetBlockValue(int nHeight, int64 nFees)
{
  int64 nSubsidy = 66.85 * COIN;

  // Subsidy halving
  nSubsidy >>= (nHeight / 100010);

  return nSubsidy + nFees;
}

namespace OMNI_Checkpoints
{
  typedef std::map<int, uint256> MapCheckpoints;

  //
  // What makes a good checkpoint block?
  // + Is surrounded by blocks with reasonable timestamps
  //   (no blocks before with a timestamp after, none after with
  //    timestamp before)
  // + Contains no strange transactions
  //
  static MapCheckpoints mapCheckpoints =
    boost::assign::map_list_of
    ( 0, uint256("0x721abe3814e15f1ab50514c8b7fffa7578c1f35aa915275ee91f4cb8d02be5c4") )
/*
    ( 1, uint256("0xec9c4d88a04ede4cd777234ac504084c36cb25080c45b4741e2cfc0d5994359a"))
    ( 50, uint256("0x253e145aae6b516ac47b9f6855675bea6f589922b74195cee77b31df1ebbc8c7"))
    ( 3000, uint256("0xb0bf45beaad4446c666158baee04488267e622fabc49e6686b798ccd122018fe"))
    ( 8000, uint256("0xde808d01865606385726824fd9f1466aacb94f233cd9713dc989333bcea15312"))
    ( 10000, uint256("0xb5bab4cfa3e92985302a95afeb1b42755d6c240e73af61deb2599cb72aba991e"))
    ( 20000, uint256("0x2f35019fbf04de7287aaa18b4010d2317779aac0a875183ff52934b8a3fee685"))
    ( 135798, uint256("0xbd8423b7e21e1422953008db6ab7197b71b4cfabb9d9e69cc0cbcdcd7dd86b30"))
    ( 1000, uint256("0xa59b03d739edd29c98cf563a1f7b57e7da8306abcae4e18397bd1e320fa79007"))
    ( 100000, uint256("0x9376d399b8b3f34549d05b6858f4cba534e78cba2306c414117dcaa057c23081"))
    ( 250000, uint256("0x7e86b4d451fcfdf4c59e7f0a8081b33366a50a82b276073c55b758d7769333bf"))
    ( 444444, uint256("0xd4b76e38fe481aef65e4dcc52703f34187aff8dcd037b1ab7abe7b7429af7d95"))
    ( 500000, uint256("0x17a3060325e40e311b42763d44574b3f63a3525f1f7644588fe00ca824c7b21e"))
    ( 750000, uint256("0xa3b1c4f90225299fef3a43851be960b49ca70e8500d1891612e2836cfbeed188"))
    ( 888888, uint256("0x96d7bf79871c8d6d887e098c444071cfda4548e502d1965e255b1b0e71c93c7a"))
    ( 1000000, uint256("0xd444bebec6a7f1345e6bee094d913bdfff0b7ae833c3e3f17b90c98fdc899aa4"))
    ( 1047382, uint256("0x7489d8515228bc90bf43ca09af944e5b3e13f43f1a15f80ae5f211533a26e791"))
    ( 1084324, uint256("0x59e7296adef10db8f517c1e05cc10b1d83925ebe53d81608a5f929ca3b98d94b"))
    ( 1087716, uint256("0xfd322ca21f75bb01a92d903fd435a48a70fc416b2f439d3eadfbd4385138b5b7"))
    ( 1087717, uint256("0x23fad9f5b12079dea9362a51d77e70f49a4d484ad93ca3b0e97fac38fd0addc5"))
    ( 1087718, uint256("0xa5c0965a380a1a5f99065472da29f5a3f1fc4c9713072597e63e402f87f1812e"))
*/
    ;


  bool CheckBlock(int nHeight, const uint256& hash)
  {
    if (fTestNet) return true; // Testnet has no checkpoints

    MapCheckpoints::const_iterator i = mapCheckpoints.find(nHeight);
    if (i == mapCheckpoints.end()) return true;
    return hash == i->second;
  }

  int GetTotalBlocksEstimate()
  {
    if (fTestNet) return 0;
    return mapCheckpoints.rbegin()->first;
  }

  CBlockIndex* GetLastCheckpoint(const std::map<uint256, CBlockIndex*>& mapBlockIndex)
  {
    if (fTestNet) return NULL;

    BOOST_REVERSE_FOREACH(const MapCheckpoints::value_type& i, mapCheckpoints)
    {
      const uint256& hash = i.second;
      std::map<uint256, CBlockIndex*>::const_iterator t = mapBlockIndex.find(hash);
      if (t != mapBlockIndex.end())
        return t->second;
    }
    return NULL;
  }

}

#if 0
bool omni_FetchInputs(CTransaction *tx, CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool, bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid)
{
  // FetchInputs can return false either because we just haven't seen some inputs
  // (in which case the transaction should be stored as an orphan)
  // or because the transaction is malformed (in which case the transaction should
  // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
  fInvalid = false;

  if (tx->IsCoinBase())
    return true; // Coinbase transactions have no inputs to fetch.

  for (unsigned int i = 0; i < tx->vin.size(); i++)
  {
    COutPoint prevout = tx->vin[i].prevout;
    if (inputsRet.count(prevout.hash))
      continue; // Got it already

    // Read txindex
    CTxIndex& txindex = inputsRet[prevout.hash].first;
    bool fFound = true;
    if ((fBlock || fMiner) && mapTestPool.count(prevout.hash))
    {
      // Get txindex from current proposed changes
      txindex = mapTestPool.find(prevout.hash)->second;
    }
    else
    {
      // Read txindex from txdb
      fFound = txdb.ReadTxIndex(prevout.hash, txindex);
    }
    if (!fFound && (fBlock || fMiner))
      return fMiner ? false : error(SHERR_INVAL, "FetchInputs() : %s prev tx %s index entry not found", tx->GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());

    // Read txPrev
    CTransaction& txPrev = inputsRet[prevout.hash].second;
    if (!fFound || txindex.pos == CDiskTxPos(0,0,0))
    {
      // Get prev tx from single transactions in memory
      {
        LOCK(OMNIBlock::mempool.cs);
        if (!OMNIBlock::mempool.exists(prevout.hash))
          return error(SHERR_INVAL, "FetchInputs() : %s OMNIBlock::mempool Tx prev not found %s", tx->GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
        txPrev = OMNIBlock::mempool.lookup(prevout.hash);
      }
      if (!fFound)
        txindex.vSpent.resize(txPrev.vout.size());
    }
    else
    {
      // Get prev tx from disk
      if (!txPrev.ReadFromDisk(txindex.pos))
        return error(SHERR_INVAL, "FetchInputs() : %s ReadFromDisk prev tx %s failed", tx->GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
    }
  }

  // Make sure all prevout.n's are valid:
  for (unsigned int i = 0; i < tx->vin.size(); i++)
  {
    const COutPoint prevout = tx->vin[i].prevout;
    assert(inputsRet.count(prevout.hash) != 0);
    const CTxIndex& txindex = inputsRet[prevout.hash].first;
    const CTransaction& txPrev = inputsRet[prevout.hash].second;
    if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
    {
      // Revisit this if/when transaction replacement is implemented and allows
      // adding inputs:
      fInvalid = true;
      return error(SHERR_INVAL, "FetchInputs() : %s prevout.n out of range %d %d %d prev tx %s\n%s", tx->GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(), txPrev.ToString().c_str());
    }
  }

  return true;
}
#endif

static bool omni_ConnectInputs(CTransaction *tx, MapPrevTx inputs, map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx, const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, bool fStrictPayToScriptHash=true)
{

  if (tx->IsCoinBase())
    return (true);

  // Take over previous transactions' spent pointers
  // fBlock is true when this is called from AcceptBlock when a new best-block is added to the blockchain
  // fMiner is true when called from the internal omni miner
  // ... both are false when called from CTransaction::AcceptToMemoryPool

  int64 nValueIn = 0;
  int64 nFees = 0;
  for (unsigned int i = 0; i < tx->vin.size(); i++)
  {
    COutPoint prevout = tx->vin[i].prevout;
    assert(inputs.count(prevout.hash) > 0);
    CTxIndex& txindex = inputs[prevout.hash].first;
    CTransaction& txPrev = inputs[prevout.hash].second;

    if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
      return error(SHERR_INVAL, "ConnectInputs() : %s prevout.n out of range %d %d %d prev tx %s\n%s", tx->GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(), txPrev.ToString().c_str());

    // If prev is coinbase, check that it's matured
    if (txPrev.IsCoinBase())
      for (const CBlockIndex* pindex = pindexBlock; pindex && pindexBlock->nHeight - pindex->nHeight < OMNI_COINBASE_MATURITY; pindex = pindex->pprev)
        //if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
        if (pindex->nHeight == txindex.pos.nBlockPos)// && pindex->nFile == txindex.pos.nFile)
          return error(SHERR_INVAL, "ConnectInputs() : tried to spend coinbase at depth %d", pindexBlock->nHeight - pindex->nHeight);

    // Check for negative or overflow input values
    nValueIn += txPrev.vout[prevout.n].nValue;
    if (!MoneyRange(OMNI_COIN_IFACE, txPrev.vout[prevout.n].nValue) || !MoneyRange(OMNI_COIN_IFACE, nValueIn))
      return error(SHERR_INVAL, "ConnectInputs() : txin values out of range");

  }
  // The first loop above does all the inexpensive checks.
  // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
  // Helps prevent CPU exhaustion attacks.
  for (unsigned int i = 0; i < tx->vin.size(); i++)
  {
    COutPoint prevout = tx->vin[i].prevout;
    assert(inputs.count(prevout.hash) > 0);
    CTxIndex& txindex = inputs[prevout.hash].first;
    CTransaction& txPrev = inputs[prevout.hash].second;

    /* this coin has been marked as spent. ensure this is not a re-write of the same transaction. */
    if (tx->IsSpentTx(txindex.vSpent[prevout.n])) {
      if (fMiner) return false;
      return error(SHERR_INVAL, "(omni) ConnectInputs: %s prev tx (%s) already used at %s", tx->GetHash().GetHex().c_str(), txPrev.GetHash().GetHex().c_str(), txindex.vSpent[prevout.n].ToString().c_str());
    }

    // Skip ECDSA signature verification when connecting blocks (fBlock=true)
    // before the last blockchain checkpoint. This is safe because block merkle hashes are
    // still computed and checked, and any change will be caught at the next checkpoint.
    if (!(fBlock && (GetBestHeight(OMNI_COIN_IFACE < OMNI_Checkpoints::GetTotalBlocksEstimate()))))
    {
      // Verify signature
      if (!VerifySignature(txPrev, *tx, i, fStrictPayToScriptHash, 0))
      {
        // only during transition phase for P2SH: do not invoke anti-DoS code for
        // potentially old clients relaying bad P2SH transactions
        if (fStrictPayToScriptHash && VerifySignature(txPrev, *tx, i, false, 0))
          return error(SHERR_INVAL, "ConnectInputs() : %s P2SH VerifySignature failed", tx->GetHash().ToString().substr(0,10).c_str());

        return error(SHERR_INVAL, "ConnectInputs() : %s VerifySignature failed", tx->GetHash().ToString().substr(0,10).c_str());
      }
    }

    // Mark outpoints as spent
    txindex.vSpent[prevout.n] = posThisTx;

    // Write back
    if (fBlock || fMiner)
    {
      mapTestPool[prevout.hash] = txindex;
    }
  }

  if (nValueIn < tx->GetValueOut())
    return error(SHERR_INVAL, "ConnectInputs() : %s value in < value out", tx->GetHash().ToString().substr(0,10).c_str());

  // Tally transaction fees
  int64 nTxFee = nValueIn - tx->GetValueOut();
  if (nTxFee < 0)
    return error(SHERR_INVAL, "ConnectInputs() : %s nTxFee < 0", tx->GetHash().ToString().substr(0,10).c_str());
  nFees += nTxFee;
  if (!MoneyRange(OMNI_COIN_IFACE, nFees))
    return error(SHERR_INVAL, "ConnectInputs() : nFees out of range");

  return true;
}

CBlock* omni_CreateNewBlock(CReserveKey& reservekey)
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);
  CBlockIndex* pindexPrev = GetBestBlockIndex(iface);

  // Create new block
  //auto_ptr<CBlock> pblock(new CBlock());
  auto_ptr<OMNIBlock> pblock(new OMNIBlock());
  if (!pblock.get())
    return NULL;

  // Create coinbase tx
  CTransaction txNew;
  txNew.vin.resize(1);
  txNew.vin[0].prevout.SetNull();
  txNew.vout.resize(1);
  txNew.vout[0].scriptPubKey << reservekey.GetReservedKey() << OP_CHECKSIG;

  // Add our coinbase tx as first transaction
  pblock->vtx.push_back(txNew);

  // Collect memory pool transactions into the block
  int64 nFees = 0;
  {
    LOCK2(cs_main, OMNIBlock::mempool.cs);
    OMNITxDB txdb; 

    // Priority order to process transactions
    list<OMNIOrphan> vOrphan; // list memory doesn't move
    map<uint256, vector<OMNIOrphan*> > mapDependers;
    multimap<double, CTransaction*> mapPriority;
    for (map<uint256, CTransaction>::iterator mi = OMNIBlock::mempool.mapTx.begin(); mi != OMNIBlock::mempool.mapTx.end(); ++mi)
    {
      CTransaction& tx = (*mi).second;
      if (tx.IsCoinBase() || !tx.IsFinal(OMNI_COIN_IFACE))
        continue;

      OMNIOrphan* porphan = NULL;
      double dPriority = 0;
      BOOST_FOREACH(const CTxIn& txin, tx.vin)
      {
        // Read prev transaction
        CTransaction txPrev;
        if (!txPrev.ReadTx(OMNI_COIN_IFACE, txin.prevout.hash)) {
          // Has to wait for dependencies
          if (!porphan)
          {
            // Use list for automatic deletion
            vOrphan.push_back(OMNIOrphan(&tx));
            porphan = &vOrphan.back();
          }
          mapDependers[txin.prevout.hash].push_back(porphan);
          porphan->setDependsOn.insert(txin.prevout.hash);
          continue;
        }

if (txPrev.vout.size() <= txin.prevout.n) {
fprintf(stderr, "DEBUG: omni_CreateNewBlock: txPrev.vout.size() %d <= txin.prevout.n %d [tx %s]\n", 
 txPrev.vout.size(),
 txin.prevout.n,
txPrev.GetHash().GetHex().c_str());
continue;
}


        int64 nValueIn = txPrev.vout[txin.prevout.n].nValue;

        // Read block header
        int nConf = GetTxDepthInMainChain(iface, txPrev.GetHash());

        dPriority += (double)nValueIn * nConf;

        if (fDebug && GetBoolArg("-printpriority"))
          printf("priority     nValueIn=%-12"PRI64d" nConf=%-5d dPriority=%-20.1f\n", nValueIn, nConf, dPriority);
      }

      // Priority is sum(valuein * age) / txsize
      dPriority /= ::GetSerializeSize(tx, SER_NETWORK, OMNI_PROTOCOL_VERSION);

      if (porphan)
        porphan->dPriority = dPriority;
      else
        mapPriority.insert(make_pair(-dPriority, &(*mi).second));

      if (fDebug && GetBoolArg("-printpriority"))
      {
        printf("priority %-20.1f %s\n%s", dPriority, tx.GetHash().ToString().substr(0,10).c_str(), tx.ToString().c_str());
        if (porphan)
          porphan->print();
        printf("\n");
      }
    }

    // Collect transactions into block
    map<uint256, CTxIndex> mapTestPool;
    uint64 nBlockSize = 1000;
    uint64 nBlockTx = 0;
    int nBlockSigOps = 100;
    while (!mapPriority.empty())
    {
      // Take highest priority transaction off priority queue
      double dPriority = -(*mapPriority.begin()).first;
      CTransaction& tx = *(*mapPriority.begin()).second;
      mapPriority.erase(mapPriority.begin());

      // Size limits
      unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, OMNI_PROTOCOL_VERSION);
      if (nBlockSize + nTxSize >= MAX_BLOCK_SIZE_GEN(iface))
        continue;

      // Legacy limits on sigOps:
      unsigned int nTxSigOps = tx.GetLegacySigOpCount();
      if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS(iface))
        continue;

      // Transaction fee required depends on block size
      // omnid: Reduce the exempted free transactions to 500 bytes (from Bitcoin's 3000 bytes)
      bool fAllowFree = (nBlockSize + nTxSize < 1500 || CTransaction::AllowFree(dPriority));
      int64 nMinFee = tx.GetMinFee(nBlockSize, fAllowFree, GMF_BLOCK);

      // Connecting shouldn't fail due to dependency on other memory pool transactions
      // because we're already processing them in order of dependency
      map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
      MapPrevTx mapInputs;
      bool fInvalid;
      if (!tx.FetchInputs(txdb, mapTestPoolTmp, NULL, true, mapInputs, fInvalid))
        continue;

      int64 nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
      if (nTxFees < nMinFee)
        continue;

      nTxSigOps += tx.GetP2SHSigOpCount(mapInputs);
      if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS(iface))
        continue;

      if (!omni_ConnectInputs(&tx, mapInputs, mapTestPoolTmp, CDiskTxPos(0,0,0), pindexPrev, false, true))
        continue;
      mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(0,0,0), tx.vout.size());
      swap(mapTestPool, mapTestPoolTmp);

      // Added
      pblock->vtx.push_back(tx);
      nBlockSize += nTxSize;
      ++nBlockTx;
      nBlockSigOps += nTxSigOps;
      nFees += nTxFees;

      // Add transactions that depend on this one to the priority queue
      uint256 hash = tx.GetHash();
      if (mapDependers.count(hash))
      {
        BOOST_FOREACH(OMNIOrphan* porphan, mapDependers[hash])
        {
          if (!porphan->setDependsOn.empty())
          {
            porphan->setDependsOn.erase(hash);
            if (porphan->setDependsOn.empty())
              mapPriority.insert(make_pair(-porphan->dPriority, porphan->ptx));
          }
        }
      }
    }

    txdb.Close();

#if 0
    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;
    //printf("CreateNewBlock(): total size %lu\n", nBlockSize);
#endif

  }
  pblock->vtx[0].vout[0].nValue = omni_GetBlockValue(pindexPrev->nHeight+1, nFees);

  // Fill in header
  pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
  pblock->hashMerkleRoot = pblock->BuildMerkleTree();
  pblock->UpdateTime(pindexPrev);
  pblock->nBits          = pblock->GetNextWorkRequired(pindexPrev);
  pblock->nNonce         = 0;

  return pblock.release();
}


bool omni_CreateGenesisBlock()
{
  blkidx_t *blockIndex = GetBlockTable(OMNI_COIN_IFACE);
  bool ret;

  if (blockIndex->count(omni_hashGenesisBlock) != 0)
    return (true); /* already created */

  // Genesis block
  const char* pszTimestamp = "OMNI founded 1/1/2014";
  CTransaction txNew;
  txNew.vin.resize(1);
  txNew.vout.resize(1);
  txNew.vin[0].scriptSig = CScript() << 486604799 << CBigNum(4) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
  txNew.vout[0].nValue = 50 * COIN;
  txNew.vout[0].scriptPubKey = CScript() << ParseHex("04a5814813115273a109cff99907ba4a05d951873dae7acb6c973d0c9e7c88911a3dbc9aa600deac241b91707e7b4ffb30ad91c8e56e695a1ddf318592988afe0a") << OP_CHECKSIG;
  OMNIBlock block;
  block.vtx.push_back(txNew);
  block.hashPrevBlock = 0;
  block.hashMerkleRoot = block.BuildMerkleTree();
  block.nVersion = 1;
  block.nTime    = 1365048244;
  block.nBits    = 0x1e0ffff0;
  block.nNonce   = 134453;

  block.print();
  if (block.GetHash() != omni_hashGenesisBlock)
    return (false);
  if (block.hashMerkleRoot != uint256("0x1f42509b6d35a6aa60af4ec9b98d8ce4ffbe46c076d4c2da933e87550ab775f2"))
    return (false);

  if (!block.WriteBlock(0)) {
    return (false);
  }

  ret = block.AddToBlockIndex();
  if (!ret)
    return (false);

  OMNITxDB txdb;
  block.SetBestChain(txdb, (*blockIndex)[omni_hashGenesisBlock]);
  txdb.Close();

  return (true);
}











static bool omni_IsFromMe(CTransaction& tx)
{
  BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
    if (pwallet->IsFromMe(tx))
      return true;
  return false;
}

static void omni_EraseFromWallets(uint256 hash)
{
  BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
    pwallet->EraseFromWallet(hash);
}

bool OMNI_CTxMemPool::accept(CTxDB& txdb, CTransaction &tx, bool fCheckInputs, bool* pfMissingInputs)
{
  if (pfMissingInputs)
    *pfMissingInputs = false;

  if (!tx.CheckTransaction(OMNI_COIN_IFACE))
    return error(SHERR_INVAL, "CTxMemPool::accept() : CheckTransaction failed");

  // Coinbase is only valid in a block, not as a loose transaction
  if (tx.IsCoinBase())
    return error(SHERR_INVAL, "CTxMemPool::accept() : coinbase as individual tx");

  // To help v0.1.5 clients who would see it as a negative number
  if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
    return error(SHERR_INVAL, "CTxMemPool::accept() : not accepting nLockTime beyond 2038 yet");

  // Rather not work on nonstandard transactions (unless -testnet)
  if (!fTestNet && !tx.IsStandard())
    return error(SHERR_INVAL, "CTxMemPool::accept() : nonstandard transaction type");

  // Do we already have it?
  uint256 hash = tx.GetHash();
  {
    LOCK(cs);
    if (mapTx.count(hash))
      return false;
  }
  if (fCheckInputs)
    if (txdb.ContainsTx(hash))
      return false;

  // Check for conflicts with in-memory transactions
  CTransaction* ptxOld = NULL;
  for (unsigned int i = 0; i < tx.vin.size(); i++)
  {
    COutPoint outpoint = tx.vin[i].prevout;
    if (mapNextTx.count(outpoint))
    {
      // omni disallow's replacement of previous tx
      return false;
    }
  }

  if (fCheckInputs)
  {
    MapPrevTx mapInputs;
    map<uint256, CTxIndex> mapUnused;
    bool fInvalid = false;
    if (!tx.FetchInputs(txdb, mapUnused, NULL, false, mapInputs, fInvalid))
    {
      if (fInvalid)
        return error(SHERR_INVAL, "CTxMemPool::accept() : FetchInputs found invalid tx %s", hash.ToString().substr(0,10).c_str());
      if (pfMissingInputs)
        *pfMissingInputs = true;
      return false;
    }

    // Check for non-standard pay-to-script-hash in inputs
    if (!tx.AreInputsStandard(mapInputs) && !fTestNet)
      return error(SHERR_INVAL, "CTxMemPool::accept() : nonstandard transaction input");

    // Note: if you modify this code to accept non-standard transactions, then
    // you should add code here to check that the transaction does a
    // reasonable number of ECDSA signature verifications.

    int64 nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
    unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, OMNI_PROTOCOL_VERSION);

    // Don't accept it if it can't get into a block
    if (nFees < tx.GetMinFee(1000, true, GMF_RELAY))
      return error(SHERR_INVAL, "CTxMemPool::accept() : not enough fees");

    // Continuously rate-limit free transactions
    // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
    // be annoying or make other's transactions take longer to confirm.
    if (nFees < OMNI_MIN_RELAY_TX_FEE)
    {
      static CCriticalSection cs;
      static double dFreeCount;
      static int64 nLastTime;
      int64 nNow = GetTime();

      {
        LOCK(cs);
        // Use an exponentially decaying ~10-minute window:
        dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
        nLastTime = nNow;
        // -limitfreerelay unit is thousand-bytes-per-minute
        // At default rate it would take over a month to fill 1GB
        if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000 && !omni_IsFromMe(tx))
          return error(SHERR_INVAL, "CTxMemPool::accept() : free transaction rejected by rate limiter");
        Debug("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
        dFreeCount += nSize;
      }
    }

    // Check against previous transactions
    // This is done last to help prevent CPU exhaustion denial-of-service attacks.

    if (!omni_ConnectInputs(&tx, mapInputs, mapUnused, CDiskTxPos(0,0,0), GetBestBlockIndex(OMNI_COIN_IFACE), false, false))
    {
      return error(SHERR_INVAL, "CTxMemPool::accept() : ConnectInputs failed %s", hash.ToString().substr(0,10).c_str());
    }
  }

  // Store transaction in memory
  {
    LOCK(cs);
    if (ptxOld)
    {
      Debug("CTxMemPool::accept() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
      remove(*ptxOld);
    }
    addUnchecked(hash, tx);
  }

  ///// are we sure this is ok when loading transactions or restoring block txes
  // If updated, erase old tx from wallet
  if (ptxOld)
    omni_EraseFromWallets(ptxOld->GetHash());

  Debug("(omni) mempool accepted %s (pool-size %u)\n",
      hash.ToString().c_str(), mapTx.size());
  return true;
}

bool OMNI_CTxMemPool::addUnchecked(const uint256& hash, CTransaction &tx)
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);

  // Add to memory pool without checking anything.  Don't call this directly,
  // call CTxMemPool::accept to properly check the transaction first.
  {
    mapTx[hash] = tx;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
      mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
    STAT_TX_ACCEPTS(iface)++;
  }
  return true;
}


bool OMNI_CTxMemPool::remove(CTransaction &tx)
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);

  // Remove transaction from memory pool
  {
    LOCK(cs);
    uint256 hash = tx.GetHash();
    if (mapTx.count(hash))
    {
      BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapNextTx.erase(txin.prevout);
      mapTx.erase(hash);
      STAT_TX_ACCEPTS(iface)++;
    }
  }
  return true;
}

void OMNI_CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}

uint256 omni_GetOrphanRoot(const CBlock* pblock)
{

  // Work back to the first block in the orphan chain
  while (OMNI_mapOrphanBlocks.count(pblock->hashPrevBlock))
    pblock = OMNI_mapOrphanBlocks[pblock->hashPrevBlock];
  return pblock->GetHash();

}



/** minimum amount of work that could possibly be required nTime after minimum work required was nBase */
unsigned int omni_ComputeMinWork(unsigned int nBase, int64 nTime)
{
  // Testnet has min-difficulty blocks
  // after omni_nTargetSpacing*2 time between blocks:
  if (fTestNet && nTime > omni_nTargetSpacing*2)
    return omni_bnProofOfWorkLimit.GetCompact();

  CBigNum bnResult;
  bnResult.SetCompact(nBase);
  while (nTime > 0 && bnResult < omni_bnProofOfWorkLimit)
  {
    // Maximum 400% adjustment...
    bnResult *= 4;
    // ... in best-case exactly 4-times-normal target time
    nTime -= omni_nTargetTimespan*4;
  }
  if (bnResult > omni_bnProofOfWorkLimit)
    bnResult = omni_bnProofOfWorkLimit;
  return bnResult.GetCompact();
}

bool omni_ProcessBlock(CNode* pfrom, CBlock* pblock)
{
  int ifaceIndex = OMNI_COIN_IFACE;
  CIface *iface = GetCoinByIndex(ifaceIndex);
  blkidx_t *blockIndex = GetBlockTable(ifaceIndex); 
  shtime_t ts;

  // Check for duplicate
  uint256 hash = pblock->GetHash();

  if (blockIndex->count(hash))
    return Debug("ProcessBlock() : already have block %s", hash.GetHex().c_str());
  if (OMNI_mapOrphanBlocks.count(hash))
    return Debug("ProcessBlock() : already have block (orphan) %s", hash.ToString().substr(0,20).c_str());

  // Preliminary checks
  if (!pblock->CheckBlock()) {
pblock->print();
    return error(SHERR_INVAL, "ProcessBlock() : CheckBlock FAILED");
  }

  CBlockIndex* pcheckpoint = OMNI_Checkpoints::GetLastCheckpoint(*blockIndex);
  if (pcheckpoint && pblock->hashPrevBlock != GetBestBlockChain(iface))
  {
    // Extra checks to prevent "fill up memory by spamming with bogus blocks"
    int64 deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
    if (deltaTime < 0)
    {
      if (pfrom)
        pfrom->Misbehaving(100);
pblock->print();
      return error(SHERR_INVAL, "ProcessBlock() : block with timestamp before last checkpoint");
    }
    CBigNum bnNewBlock;
    bnNewBlock.SetCompact(pblock->nBits);
    CBigNum bnRequired;
    bnRequired.SetCompact(omni_ComputeMinWork(pcheckpoint->nBits, deltaTime));
    if (bnNewBlock > bnRequired)
    {
      if (pfrom)
        pfrom->Misbehaving(100);
      return error(SHERR_INVAL, "ProcessBlock() : block with too little proof-of-work");
    }
  }


  /* block is considered orphan when previous block or one of the transaction's input hashes is unknown. */
  if (!blockIndex->count(pblock->hashPrevBlock) ||
      !pblock->CheckTransactionInputs(OMNI_COIN_IFACE)) {
    error(SHERR_INVAL, "(omni) ProcessBlock: warning: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.GetHex().c_str());

    OMNIBlock* pblock2 = new OMNIBlock(*pblock);
    OMNI_mapOrphanBlocks.insert(make_pair(hash, pblock2));
    OMNI_mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

    // Ask this guy to fill in what we're missing
    if (pfrom) {
      pfrom->PushGetBlocks(GetBestBlockIndex(OMNI_COIN_IFACE), omni_GetOrphanRoot(pblock2));
}

    return true;
  }

  // Store to disk

  if (!pblock->AcceptBlock()) {
    iface->net_invalid = time(NULL);
    return error(SHERR_INVAL, "ProcessBlock() : AcceptBlock FAILED");
  }
  ServiceBlockEventUpdate(OMNI_COIN_IFACE);

  // Recursively process any orphan blocks that depended on this one
  vector<uint256> vWorkQueue;
  vWorkQueue.push_back(hash);
  for (unsigned int i = 0; i < vWorkQueue.size(); i++)
  {
    uint256 hashPrev = vWorkQueue[i];
    for (multimap<uint256, OMNIBlock*>::iterator mi = OMNI_mapOrphanBlocksByPrev.lower_bound(hashPrev);
        mi != OMNI_mapOrphanBlocksByPrev.upper_bound(hashPrev);
        ++mi)
    {
      CBlock* pblockOrphan = (*mi).second;
      if (pblockOrphan->AcceptBlock())
        vWorkQueue.push_back(pblockOrphan->GetHash());

      OMNI_mapOrphanBlocks.erase(pblockOrphan->GetHash());

      delete pblockOrphan;
    }
    OMNI_mapOrphanBlocksByPrev.erase(hashPrev);
  }

  return true;
}

bool omni_CheckProofOfWork(uint256 hash, unsigned int nBits)
{
  CBigNum bnTarget;
  bnTarget.SetCompact(nBits);

  /* Check range */
  if (bnTarget <= 0 || bnTarget > omni_bnProofOfWorkLimit)
    return error(SHERR_INVAL, "CheckProofOfWork() : nBits below minimum work");

  /* Check proof of work matches claimed amount */
  if (hash > bnTarget.getuint256())
    return error(SHERR_INVAL, "CheckProofOfWork() : hash doesn't match nBits");

  return true;
}

/**
 * @note These are checks that are independent of context that can be verified before saving an orphan block.
 */
bool OMNIBlock::CheckBlock()
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);

  if (vtx.empty()) {
    return (trust(-100, "(omni) CheckBlock: block submitted with zero transactions"));
  }
  if (vtx.size() > OMNI_MAX_BLOCK_SIZE) {
    return (trust(-100, "(omni) CheckBlock: vtx.size (%d) > %d", vtx.size(), OMNI_MAX_BLOCK_SIZE));
  }
  size_t ser_len = ::GetSerializeSize(*this, SER_NETWORK, OMNI_PROTOCOL_VERSION);
  if (ser_len > OMNI_MAX_BLOCK_SIZE) {
    return (trust(-100, "(omni) CheckBlock: block size (%d) > max-block-size (%d)", ser_len, OMNI_MAX_BLOCK_SIZE));
  }
#if 0
  if (vtx.empty() || vtx.size() > OMNI_MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, OMNI_PROTOCOL_VERSION) > OMNI_MAX_BLOCK_SIZE) {
print();
    return error(SHERR_INVAL, "OMNI::CheckBlock: size limits failed");
  }
#endif

  if (!vtx[0].IsCoinBase()) {
    return (trust(-100, "(omni) ChecKBlock: first transaction is not coin base"));
  }

  // Check proof of work matches claimed amount
  if (!omni_CheckProofOfWork(GetPoWHash(), nBits)) {
    return (trust(-50, "(omni) CheckBlock: proof of work failed"));
  }

  // Check timestamp
  if (GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60) {
    return error(SHERR_INVAL, "CheckBlock() : block timestamp too far in the future");
  }

  // First transaction must be coinbase, the rest must not be
  for (unsigned int i = 1; i < vtx.size(); i++) {
    if (vtx[i].IsCoinBase()) {
      return (trust(-100, "(omni) CheckBlock: more than one coinbase in transaction"));
    }
  }

  // Check transactions
  BOOST_FOREACH(CTransaction& tx, vtx) {
    if (!tx.CheckTransaction(OMNI_COIN_IFACE)) {
      return (trust(-1, "(omni) ChecKBlock: transaction verification failure"));
    }
  }

  // Check for duplicate txids. This is caught by ConnectInputs(),
  // but catching it earlier avoids a potential DoS attack:
  set<uint256> uniqueTx;
  BOOST_FOREACH(const CTransaction& tx, vtx)
  {
    uniqueTx.insert(tx.GetHash());
  }
  if (uniqueTx.size() != vtx.size()) {
    return (trust(-100, "(omni) CheckBlock: duplicate transactions"));
  }

  unsigned int nSigOps = 0;
  BOOST_FOREACH(const CTransaction& tx, vtx)
  {
    nSigOps += tx.GetLegacySigOpCount();
  }
  if (nSigOps > MAX_BLOCK_SIGOPS(iface)) {
    return (trust(-100, "(omni) CheckBlock: out-of-bounds SigOpCount"));
  }

  // Check merkleroot
  if (hashMerkleRoot != BuildMerkleTree()) {
    return (trust(-100, "(omni0 CheckBlock: invalid merkle root hash"));
  }

  return true;
}




bool static OMNI_Reorganize(CTxDB& txdb, CBlockIndex* pindexNew, OMNI_CTxMemPool *mempool)
{
  char errbuf[1024];

 // Find the fork
  CBlockIndex* pindexBest = GetBestBlockIndex(OMNI_COIN_IFACE);
  CBlockIndex* pfork = pindexBest;
  CBlockIndex* plonger = pindexNew;
  while (pfork != plonger)
  {
    while (plonger->nHeight > pfork->nHeight)
      if (!(plonger = plonger->pprev))
        return error(SHERR_INVAL, "Reorganize() : plonger->pprev is null");
    if (pfork == plonger)
      break;
    if (!pfork->pprev) {
      sprintf(errbuf, "OMNI_Reorganize: no previous chain for '%s' height %d\n", pfork->GetBlockHash().GetHex().c_str(), pfork->nHeight); 
      return error(SHERR_INVAL, errbuf);
    }
    pfork = pfork->pprev;
  }


  // List of what to disconnect
  vector<CBlockIndex*> vDisconnect;
  for (CBlockIndex* pindex = GetBestBlockIndex(OMNI_COIN_IFACE); pindex != pfork; pindex = pindex->pprev)
    vDisconnect.push_back(pindex);

  // List of what to connect
  vector<CBlockIndex*> vConnect;
  for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
    vConnect.push_back(pindex);
  reverse(vConnect.begin(), vConnect.end());

pindexBest = GetBestBlockIndex(OMNI_COIN_IFACE);
Debug("REORGANIZE: Disconnect %i blocks; %s..%s\n", vDisconnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(), pindexBest->GetBlockHash().ToString().substr(0,20).c_str());
Debug("REORGANIZE: Connect %i blocks; %s..%s\n", vConnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->GetBlockHash().ToString().substr(0,20).c_str());

  // Disconnect shorter branch
  vector<CTransaction> vResurrect;
  BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
  {
    OMNIBlock block;
    if (!block.ReadFromDisk(pindex)) {
      if (!block.ReadArchBlock(pindex->GetBlockHash()))
        return error(SHERR_IO, "OMNI_Reorganize: Disconnect: block hash '%s' [height %d] could not be loaded.", pindex->GetBlockHash().GetHex().c_str(), pindex->nHeight);
    }
    if (!block.DisconnectBlock(txdb, pindex))
      return error(SHERR_INVAL, "Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString().c_str());

    // Queue memory transactions to resurrect
    BOOST_FOREACH(const CTransaction& tx, block.vtx)
      if (!tx.IsCoinBase())
        vResurrect.push_back(tx);
  }

  // Connect longer branch
  vector<CTransaction> vDelete;
  for (unsigned int i = 0; i < vConnect.size(); i++)
  {
    CBlockIndex* pindex = vConnect[i];
    OMNIBlock block;
    if (!block.ReadFromDisk(pindex)) {
      if (!block.ReadArchBlock(pindex->GetBlockHash()))
        return error(SHERR_IO, "OMNI_Reorganize: Connect: block hash '%s' [height %d] could not be loaded.", pindex->GetBlockHash().GetHex().c_str(), pindex->nHeight);
    }

    if (!block.ConnectBlock(txdb, pindex))
    {
      // Invalid block
      return error(SHERR_INVAL, "Reorganize() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());
    }

    // Queue memory transactions to delete
    BOOST_FOREACH(const CTransaction& tx, block.vtx)
      vDelete.push_back(tx);
  }

  if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
    return error(SHERR_INVAL, "Reorganize() : WriteHashBestChain failed");

  // Make sure it's successfully written to disk before changing memory structure
  if (!txdb.TxnCommit())
    return error(SHERR_INVAL, "Reorganize() : TxnCommit failed");

  // Disconnect shorter branch
  BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    if (pindex->pprev)
      pindex->pprev->pnext = NULL;

  // Connect longer branch
  BOOST_FOREACH(CBlockIndex* pindex, vConnect)
    if (pindex->pprev)
      pindex->pprev->pnext = pindex;

  // Resurrect memory transactions that were in the disconnected branch
  BOOST_FOREACH(CTransaction& tx, vResurrect)
    tx.AcceptToMemoryPool(txdb, false);

  // Delete redundant memory transactions that are in the connected branch
  BOOST_FOREACH(CTransaction& tx, vDelete)
    mempool->remove(tx);

  return true;
}

void OMNIBlock::InvalidChainFound(CBlockIndex* pindexNew)
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);

  if (pindexNew->bnChainWork > bnBestInvalidWork)
  {
    bnBestInvalidWork = pindexNew->bnChainWork;
    OMNITxDB txdb;
    txdb.WriteBestInvalidWork(bnBestInvalidWork);
    txdb.Close();
    //    uiInterface.NotifyBlocksChanged();
  }
  error(SHERR_INVAL, "OMNI: InvalidChainFound: invalid block=%s  height=%d  work=%s  date=%s\n",
      pindexNew->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->nHeight,
      pindexNew->bnChainWork.ToString().c_str(), DateTimeStrFormat("%x %H:%M:%S",
        pindexNew->GetBlockTime()).c_str());
  CBlockIndex *pindexBest = GetBestBlockIndex(OMNI_COIN_IFACE); 
  fprintf(stderr, "critical: InvalidChainFound:  current best=%s  height=%d  work=%s  date=%s\n", 
GetBestBlockChain(iface).ToString().substr(0,20).c_str(), GetBestHeight(OMNI_COIN_IFACE), bnBestChainWork.ToString().c_str(), DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());
  if (pindexBest && bnBestInvalidWork > bnBestChainWork + pindexBest->GetBlockWork() * 6)
    unet_log(OMNI_COIN_IFACE, "InvalidChainFound: WARNING: Displayed transactions may not be correct!  You may need to upgrade, or other nodes may need to upgrade.\n");
}

bool OMNIBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew)
{
  uint256 hash = GetHash();

  // Adding to current best branch
  if (!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
  {
    txdb.TxnAbort();
    InvalidChainFound(pindexNew);
    return false;
  }
  if (!txdb.TxnCommit())
    return error(SHERR_IO, "SetBestChain() : TxnCommit failed");

  // Add to current best branch
  pindexNew->pprev->pnext = pindexNew;

  // Delete redundant memory transactions
  BOOST_FOREACH(CTransaction& tx, vtx)
    mempool.remove(tx);

  return true;
}

// notify wallets about a new best chain
void static OMNI_SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

#if 0
/* if block is over one day old than consider it history. */
static bool OMNI_IsInitialBlockDownload()
{

  if (pindexBest == NULL || GetBestHeight(OMNI_COIN_IFACE) < OMNI_Checkpoints::GetTotalBlocksEstimate())
    return true;

  static int64 nLastUpdate;
  static CBlockIndex* pindexLastBest;
  if (pindexBest != pindexLastBest)
  {
    pindexLastBest = pindexBest;
    nLastUpdate = GetTime();
  }
  return (GetTime() - nLastUpdate < 15 &&
      pindexBest->GetBlockTime() < GetTime() - 24 * 60 * 60);
}
#endif

#if 0
/* not currently used */
bool omni_Reindex(CTxDB& txdb, CBlockIndex *pindexNew)
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);
  bc_t *bc = GetBlockChain(iface);
  vector<CTransaction> vResurrect;
  vector<CTransaction> vDelete;
  vector<CBlockIndex*> vConnect;
  map<int, CBlock *>mLinkBlock; 
  map<int, CBlockIndex *>mLinkIndex;
  map<int, CBlock *>mUnlinkBlock; 
  map<int, CBlockIndex *>mUnlinkIndex;
  CBlockIndex *pindexIntermediate = pindexNew;
  CBlockIndex *pindex;
  char errbuf[1024];
  unsigned int nHeight;
  unsigned int nLinkHeight;
  unsigned int nUnlinkHeight;
  unsigned int nStartHeight;

  CBlockIndex* pindexBest = GetBestBlockIndex(OMNI_COIN_IFACE);
  if (!pindexBest)
    return error(SHERR_INVAL, "no established block-chain.");
  fprintf(stderr, "DEBUG: OMNIBlock::Reindex: block height %d\n", pindexNew->nHeight);

  while (pindexIntermediate->pprev && 
      pindexIntermediate->pprev->bnChainWork > pindexBest->bnChainWork)
  {
    pindexIntermediate = pindexIntermediate->pprev;
  }

  // Find the fork
  CBlockIndex* pfork = pindexBest;
  CBlockIndex* plonger = pindexIntermediate;
  while (pfork != plonger)
  {
    while (plonger->nHeight > pfork->nHeight)
      if (!(plonger = plonger->pprev))
        return error(SHERR_INVAL, "Reorganize() : plonger->pprev is null");
    if (pfork == plonger)
      break;
    if (!pfork->pprev) {
      sprintf(errbuf, "OMNI_Reorganize: no previous chain for '%s' height %d\n", pfork->GetBlockHash().GetHex().c_str(), pfork->nHeight); 
      return error(SHERR_INVAL, errbuf);
    }
    pfork = pfork->pprev;
  }
  nStartHeight = pfork->nHeight;


  /* gather unlink chain */
  for (pindex = pindexBest; pindex->pprev && pindex->nHeight > nStartHeight; pindex = pindex->pprev) {
    mUnlinkIndex[pindex->nHeight] = pindex;
  }
  for (nHeight = pindexBest->nHeight; nHeight > nStartHeight; nHeight--) {
    CBlock *block = GetBlockByHeight(iface, nHeight);
    if (!block) {
      sprintf(errbuf, "no block height %d established in block-chain.");
      unet_log(OMNI_COIN_IFACE, errbuf);
      continue;
    }
    mUnlinkBlock[nHeight] = block;
  }
  nUnlinkHeight = pindexBest->nHeight;

  /* gather new chain */
  for (pindex = pindexNew; pindex != pfork; pindex = pindex->pprev) {
    vConnect.push_back(pindex);
  }
  nLinkHeight = nStartHeight + vConnect.size(); 

  nHeight = nLinkHeight; 
  BOOST_FOREACH(CBlockIndex* dis_pindex, vConnect) {
    CBlock *block;

    block = GetArchBlockByHash(iface, dis_pindex->GetBlockHash()); 
    if (!block)
      block = GetBlockByHash(iface, dis_pindex->GetBlockHash()); 

#if 0
    if (!block)
      return error(SHERR_INVAL, "OMNI::Reindex/Connect: unable to load block hash '%s' for new chain height %d.", dis_pindex->GetBlockHash().c_str(), nHeight);
#endif

    mLinkBlock[nHeight] = block;
    mLinkIndex[nHeight] = pindex; 

    nHeight--;
  }

  /* disconect (down) */
  for (nHeight = nUnlinkHeight; nHeight > nStartHeight; nHeight--) {
    pindex = mUnlinkIndex[nHeight];
    CBlock *block = mUnlinkBlock[nHeight];

    if (block && pindex) {
      block->DisconnectBlock(txdb, pindex); 
      BOOST_FOREACH(const CTransaction& tx, block->vtx) {
        if (!tx.IsCoinBase())
          vResurrect.push_back(tx);
      }
    }
    if (pindex && pindex->pprev) {
      pindex->pprev->pnext = NULL;
    }
    bc_clear(bc, nHeight);
  }

  /* connect (up) */
  pindex = pfork;
  for (nHeight = nStartHeight + 1; nHeight <= nLinkHeight; nHeight++) {
    pindex = mLinkIndex[nHeight];
    CBlock *block = mLinkBlock[nHeight];
    if (!pindex || !block)
      break;
    if (!block->ConnectBlock(txdb, pindex))
      break;
    pindex->pprev->pnext = pindex;
    BOOST_FOREACH(const CTransaction& tx, block->vtx)
      vDelete.push_back(tx);
  }

  if (!txdb.WriteHashBestChain(pindex->GetBlockHash()))
    return error(SHERR_INVAL, "OMNI:Reindex: WriteHashBestChain failed");

  if (!txdb.TxnCommit())
    return error(SHERR_INVAL, "Reorganize() : TxnCommit failed");

  // Resurrect memory transactions that were in the disconnected branch
  BOOST_FOREACH(CTransaction& tx, vResurrect)
    tx.AcceptToMemoryPool(txdb, false);

  // Delete redundant memory transactions that are in the connected branch
  BOOST_FOREACH(CTransaction& tx, vDelete)
    OMNIBlock::mempool.remove(tx);

  return true;
}
#endif

bool OMNIBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);
  uint256 hash = GetHash();
  shtime_t ts;
  bool ret;

  Debug("OMNIBlock::SetBestChain: setting best chain to block '%s' @ height %d.", pindexNew->GetBlockHash().GetHex().c_str(), pindexNew->nHeight);

  if (!txdb.TxnBegin())
    return error(SHERR_INVAL, "SetBestChain() : TxnBegin failed");

  if (OMNIBlock::pindexGenesisBlock == NULL && hash == omni_hashGenesisBlock)
  {
    txdb.WriteHashBestChain(hash);
    if (!txdb.TxnCommit())
      return error(SHERR_INVAL, "SetBestChain() : TxnCommit failed");
    OMNIBlock::pindexGenesisBlock = pindexNew;
  }
  else if (hashPrevBlock == GetBestBlockChain(iface))
  {
    if (!SetBestChainInner(txdb, pindexNew))
      return error(SHERR_INVAL, "SetBestChain() : SetBestChainInner failed");
  }
  else
  {
/* DEBUG: 060316 - reorg will try to load this block from db. */
    WriteArchBlock();

    //Reindex(pindexNew);
    // the first block in the new chain that will cause it to become the new best chain
    CBlockIndex *pindexIntermediate = pindexNew;

    // list of blocks that need to be connected afterwards
    std::vector<CBlockIndex*> vpindexSecondary;

    // Reorganize is costly in terms of db load, as it works in a single db transaction.
    // Try to limit how much needs to be done inside
    while (pindexIntermediate->pprev && pindexIntermediate->pprev->bnChainWork > GetBestBlockIndex(OMNI_COIN_IFACE)->bnChainWork)
    {
      vpindexSecondary.push_back(pindexIntermediate);
      pindexIntermediate = pindexIntermediate->pprev;
    }

    if (!vpindexSecondary.empty())
      Debug("Postponing %i reconnects\n", vpindexSecondary.size());

    // Switch to new best branch
    
    ret = OMNI_Reorganize(txdb, pindexIntermediate, &mempool);
    if (!ret) {
      txdb.TxnAbort();
      InvalidChainFound(pindexNew);
      return error(SHERR_INVAL, "SetBestChain() : Reorganize failed");
    }

    // Connect futher blocks
    BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
    {
      OMNIBlock block;
      if (!block.ReadFromDisk(pindex) &&
          !block.ReadArchBlock(pindex->GetBlockHash())) {
        error(SHERR_IO, "SetBestChain() : ReadFromDisk failed\n");
        break;
      }
      if (!txdb.TxnBegin()) {
        error(SHERR_INVAL, "SetBestChain() : TxnBegin 2 failed\n");
        break;
      }
      // errors now are not fatal, we still did a reorganisation to a new chain in a valid way
      if (!block.SetBestChainInner(txdb, pindex))
        break;
    }
  }

  // Update best block in wallet (so we can detect restored wallets)
  bool fIsInitialDownload = IsInitialBlockDownload(OMNI_COIN_IFACE);
  if (!fIsInitialDownload)
  {
    const CBlockLocator locator(OMNI_COIN_IFACE, pindexNew);
    OMNI_SetBestChain(locator);
  }

  // New best block
//  OMNIBlock::hashBestChain = hash;
  SetBestBlockIndex(OMNI_COIN_IFACE, pindexNew);
//  SetBestHeight(iface, pindexNew->nHeight);
  bnBestChainWork = pindexNew->bnChainWork;
  nTimeBestReceived = GetTime();
  STAT_TX_ACCEPTS(iface)++;

//fprintf(stderr, "DEBUG: OMNI/SetBestChain: new best=%s  height=%d  work=%s  date=%s\n", hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, bnBestChainWork.ToString().c_str(), DateTimeStrFormat("%x %H:%M:%S", OMNIBlock::pindexBest->GetBlockTime()).c_str());

  // Check the version of the last 100 blocks to see if we need to upgrade:
  if (!fIsInitialDownload)
  {
    int nUpgraded = 0;
    const CBlockIndex* pindex = GetBestBlockIndex(OMNI_COIN_IFACE);
    for (int i = 0; i < 100 && pindex != NULL; i++)
    {
      if (pindex->nVersion > CURRENT_VERSION)
        ++nUpgraded;
      pindex = pindex->pprev;
    }
    if (nUpgraded > 0)
      Debug("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, CURRENT_VERSION);
    //        if (nUpgraded > 100/2)
    // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
    //            strMiscWarning = _("Warning: this version is obsolete, upgrade required");
  }

  std::string strCmd = GetArg("-blocknotify", "");

  if (!fIsInitialDownload && !strCmd.empty())
  {
    boost::replace_all(strCmd, "%s", GetBestBlockChain(iface).GetHex());
    boost::thread t(runCommand, strCmd); // thread runs free
  }

  return true;
}

bool OMNIBlock::IsBestChain()
{
  CBlockIndex *pindexBest = GetBestBlockIndex(OMNI_COIN_IFACE);
  return (pindexBest && GetHash() == pindexBest->GetBlockHash());
}

bool OMNIBlock::AcceptBlock()
{
  return (core_AcceptBlock(this));
}

CScript OMNIBlock::GetCoinbaseFlags()
{
  return (OMNI_COINBASE_FLAGS);
}

static void omni_UpdatedTransaction(const uint256& hashTx)
{
  BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
    pwallet->UpdatedTransaction(hashTx);
}

bool OMNIBlock::ConnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{
  char errbuf[1024];

  /* "Check it again in case a previous version let a bad block in" */
#if 1 /* DEBUG: */
  if (!CheckBlock())
    return false;
#endif

  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);
  bc_t *bc = GetBlockTxChain(iface);
  unsigned int nFile = OMNI_COIN_IFACE;
  unsigned int nBlockPos = pindex->nHeight;;
  bc_hash_t b_hash;
  int err;

  // Do not allow blocks that contain transactions which 'overwrite' older transactions,
  // unless those are already completely spent.
  // If such overwrites are allowed, coinbases and transactions depending upon those
  // can be duplicated to remove the ability to spend the first instance -- even after
  // being sent to another address.
  // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
  // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
  // already refuses previously-known transaction id's entirely.
  // This rule applies to all blocks whose timestamp is after October 1, 2012, 0:00 UTC.
  int64 nBIP30SwitchTime = 1349049600;
  bool fEnforceBIP30 = (pindex->nTime > nBIP30SwitchTime);

  // BIP16 didn't become active until October 1 2012
  int64 nBIP16SwitchTime = 1349049600;
  bool fStrictPayToScriptHash = (pindex->nTime >= nBIP16SwitchTime);

  map<uint256, CTxIndex> mapQueuedChanges;
  int64 nFees = 0;
  unsigned int nSigOps = 0;
  BOOST_FOREACH(CTransaction& tx, vtx)
  {
    uint256 hashTx = tx.GetHash();
    int nTxPos;

    if (fEnforceBIP30) {
      CTxIndex txindexOld;
      if (txdb.ReadTxIndex(hashTx, txindexOld)) {
        BOOST_FOREACH(CDiskTxPos &pos, txindexOld.vSpent)
          if (tx.IsSpentTx(pos))
            return error(SHERR_INVAL, "OMNIBlock::ConnectBlock: BIP30 enforced at height %d\n", pindex->nHeight);
      }
    }

    nSigOps += tx.GetLegacySigOpCount();
    if (nSigOps > MAX_BLOCK_SIGOPS(iface)) {
      return (trust(-100, "(omni) ConnectBlock: too many sigops (%d > %d)", nSigOps, MAX_BLOCK_SIGOPS(iface)));
    }

#if 0
    memcpy(b_hash, tx.GetHash().GetRaw(), sizeof(bc_hash_t));
    err = bc_find(bc, b_hash, &nTxPos); 
    if (err) {
      return error(SHERR_INVAL, "OMNIBlock::ConncetBlock: error finding tx hash.");
    }
#endif

    MapPrevTx mapInputs;
    CDiskTxPos posThisTx(OMNI_COIN_IFACE, nBlockPos, nTxPos);
    if (!tx.IsCoinBase())
    {
      bool fInvalid;
      if (!tx.FetchInputs(txdb, mapQueuedChanges, this, false, mapInputs, fInvalid)) {
        sprintf(errbuf, "OMNI::ConnectBlock: FetchInputs failed for tx '%s' @ height %u\n", tx.GetHash().GetHex().c_str(), (unsigned int)nBlockPos);
        return error(SHERR_INVAL, errbuf);
      }

      if (fStrictPayToScriptHash)
      {
        // Add in sigops done by pay-to-script-hash inputs;
        // this is to prevent a "rogue miner" from creating
        // an incredibly-expensive-to-validate block.
        nSigOps += tx.GetP2SHSigOpCount(mapInputs);
        if (nSigOps > MAX_BLOCK_SIGOPS(iface)) {
          return (trust(-100, "(omni) ConnectBlock: too many sigops (%d > %d)", nSigOps, MAX_BLOCK_SIGOPS(iface)));
        }
      }

      nFees += tx.GetValueIn(mapInputs)-tx.GetValueOut();

      if (!omni_ConnectInputs(&tx, mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, fStrictPayToScriptHash)) {
        return error(SHERR_INVAL, "OMNIBlock::ConnectBlock: error connecting inputs.");
      }
    }

    mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());
  }

  // Write queued txindex changes
  for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
  {
    if (!txdb.UpdateTxIndex((*mi).first, (*mi).second)) {
      return error(SHERR_INVAL, "ConnectBlock() : UpdateTxIndex failed");
    }
  }

#if 0
  if (vtx.size() == 0) {
    return error(SHERR_INVAL, "OMNIBlock::ConnectBlock: vtx.size() == 0");
  }
#endif
  
  int64 nValue = omni_GetBlockValue(pindex->nHeight, nFees);
  if (vtx[0].GetValueOut() > omni_GetBlockValue(pindex->nHeight, nFees)) {
    sprintf(errbuf, "OMNI::ConnectBlock: coinbase output (%d coins) higher than expected block value @ height %d (%d coins) [block %s].\n", FormatMoney(vtx[0].GetValueOut()).c_str(), pindex->nHeight, FormatMoney(nValue).c_str(), pindex->GetBlockHash().GetHex().c_str());
    return error(SHERR_INVAL, errbuf);
  }


  if (pindex->pprev)
  {
    if (pindex->pprev->nHeight + 1 != pindex->nHeight) {
fprintf(stderr, "DEBUG: ConnectBlock: block-index for hash '%s' height changed from %d to %d.\n", pindex->GetBlockHash().GetHex().c_str(), pindex->nHeight, (pindex->pprev->nHeight + 1));
      pindex->nHeight = pindex->pprev->nHeight + 1;
    }
    if (!WriteBlock(pindex->nHeight)) {
      return (error(SHERR_INVAL, "ConnectBlock: error writing block hash '%s' to height %d\n", GetHash().GetHex().c_str(), pindex->nHeight));
    }

Debug("CONNECT: hash '%s' to height %d\n", GetHash().GetHex().c_str(), pindex->nHeight);
#if 0
    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    CDiskBlockIndex blockindexPrev(pindex->pprev);
    blockindexPrev.hashNext = pindex->GetBlockHash();
    if (!txdb.WriteBlockIndex(blockindexPrev))
      return error(SHERR_INVAL, "ConnectBlock() : WriteBlockIndex failed");
#endif
  }

  BOOST_FOREACH(CTransaction& tx, vtx)
    SyncWithWallets(iface, tx, this);

  return true;
}

bool OMNIBlock::ReadBlock(uint64_t nHeight)
{
int ifaceIndex = OMNI_COIN_IFACE;
  CIface *iface = GetCoinByIndex(ifaceIndex);
  CDataStream sBlock(SER_DISK, CLIENT_VERSION);
  size_t sBlockLen;
  unsigned char *sBlockData;
  char errbuf[1024];
  bc_t *bc;
  int err;

  bc = GetBlockChain(iface);
  if (!bc)
    return (false);

  err = bc_get(bc, nHeight, &sBlockData, &sBlockLen);
  if (err) {
    sprintf(errbuf, "CBlock::ReadBlock[height %d]: %s (sherr %d).",
      (int)nHeight, sherrstr(err), err);
    unet_log(ifaceIndex, errbuf);
    return (false);
  }

  SetNull();

  /* serialize binary data into block */
  sBlock.write((const char *)sBlockData, sBlockLen);
  sBlock >> *this;
  free(sBlockData);

  uint256 cur_hash = GetHash();
#if 0
  {
    uint256 t_hash;
    bc_hash_t b_hash;
    memcpy(b_hash, cur_hash.GetRaw(), sizeof(bc_hash_t));
    t_hash.SetRaw(b_hash);
    if (!bc_hash_cmp(t_hash.GetRaw(), cur_hash.GetRaw())) {
      fprintf(stderr, "DEBUG: ReadBlock: error comparing self-hash ('%s' / '%s')\n", cur_hash.GetHex().c_str(), t_hash.GetHex().c_str());
    }
  }
#endif
  {
    uint256 db_hash;
    bc_hash_t ret_hash;
    err = bc_get_hash(bc, nHeight, ret_hash);
    if (err) {
      return error(err, "OMNIBlock::ReadBlock: error obtaining block-chain height %d.", nHeight);
    }
    db_hash.SetRaw((unsigned int *)ret_hash);

#if 0
    if (!bc_hash_cmp(db_hash.GetRaw(), cur_hash.GetRaw())) {
      sprintf(errbuf, "CBlock::ReadBlock: hash '%s' from loaded block at pos %d has invalid hash of '%s'.", db_hash.GetHex().c_str(), nHeight, cur_hash.GetHex().c_str());
      print();
      SetNull();
      return error(SHERR_INVAL, errbuf);
    }
#endif
  }

#if 0
  if (!CheckBlock()) {
    unet_log(ifaceIndex, "CBlock::ReadBlock: block validation failure.");
    return (false);
  }
#endif

  return (true);
}

bool OMNIBlock::ReadArchBlock(uint256 hash)
{
  int ifaceIndex = OMNI_COIN_IFACE;
  CIface *iface = GetCoinByIndex(ifaceIndex);
  CDataStream sBlock(SER_DISK, CLIENT_VERSION);
  size_t sBlockLen;
  unsigned char *sBlockData;
  char errbuf[1024];
  bcsize_t nPos;
  bc_t *bc;
  int err;

  bc = GetBlockChain(iface);
  if (!bc)
    return (false);

  err = bc_arch_find(bc, hash.GetRaw(), NULL, &nPos);
  if (err)
    return false;

  err = bc_arch(bc, nPos, &sBlockData, &sBlockLen);
  if (err) {
    sprintf(errbuf, "CBlock::ReadBlock[arch-idx %d]: %s (sherr %d).",
      (int)nPos, sherrstr(err), err);
    unet_log(ifaceIndex, errbuf);
    return (false);
  }

  SetNull();

  /* serialize binary data into block */
  sBlock.write((const char *)sBlockData, sBlockLen);
  sBlock >> *this;
  free(sBlockData);

if (hash != GetHash()) {
fprintf(stderr, "DEBUG: ARCH: Invalid arch loaded:\n");
print();
return (false);
}

Debug("ARCH: loaded block '%s'\n", GetHash().GetHex().c_str());
  return (true);
}

bool OMNIBlock::IsOrphan()
{
  blkidx_t *blockIndex = GetBlockTable(OMNI_COIN_IFACE);
  uint256 hash = GetHash();

  if (blockIndex->count(hash))
    return (false);

  if (!OMNI_mapOrphanBlocks.count(hash))
    return (false);

  return (true);
}


bool omni_Truncate(uint256 hash)
{
  blkidx_t *blockIndex = GetBlockTable(OMNI_COIN_IFACE);
  CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);
  CBlockIndex *pBestIndex;
  CBlockIndex *cur_index;
  CBlockIndex *pindex;
  unsigned int nHeight;
  int err;

  if (!blockIndex || !blockIndex->count(hash))
    return error(SHERR_INVAL, "Erase: block not found in block-index.");

  cur_index = (*blockIndex)[hash];
  if (!cur_index)
    return error(SHERR_INVAL, "Erase: block not found in block-index.");

  pBestIndex = GetBestBlockIndex(iface);
  if (!pBestIndex)
    return error(SHERR_INVAL, "Erase: no block-chain established.");
  if (cur_index->nHeight > pBestIndex->nHeight)
    return error(SHERR_INVAL, "Erase: height is not valid.");

  bc_t *bc = GetBlockChain(iface);
  unsigned int nMinHeight = cur_index->nHeight;
  unsigned int nMaxHeight = (bc_idx_next(bc)-1);
    
  OMNITxDB txdb; /* OPEN */

  for (nHeight = nMaxHeight; nHeight > nMinHeight; nHeight--) {
    OMNIBlock block;
    if (block.ReadBlock(nHeight)) {
      uint256 t_hash = block.GetHash();
      if (hash == cur_index->GetBlockHash())
        break; /* bad */
      if (blockIndex->count(t_hash) != 0)
        block.DisconnectBlock(txdb, (*blockIndex)[t_hash]);
    }
  }
  for (nHeight = nMaxHeight; nHeight > nMinHeight; nHeight--) {
    bc_clear(bc, nHeight);
  }  

  SetBestBlockIndex(iface, cur_index);
  bool ret = txdb.WriteHashBestChain(cur_index->GetBlockHash());

  txdb.Close(); /* CLOSE */

  if (!ret)
    return error(SHERR_INVAL, "Truncate: WriteHashBestChain '%s' failed", hash.GetHex().c_str());

  cur_index->pnext = NULL;
  return (true);
}
bool OMNIBlock::Truncate()
{
  return (omni_Truncate(GetHash()));
}

bool OMNIBlock::VerifyCheckpoint(int nHeight)
{
  return (OMNI_Checkpoints::CheckBlock(nHeight, GetHash()));
}
uint64_t OMNIBlock::GetTotalBlocksEstimate()
{
  return ((uint64_t)OMNI_Checkpoints::GetTotalBlocksEstimate());
}

bool OMNIBlock::AddToBlockIndex()
{
CIface *iface = GetCoinByIndex(OMNI_COIN_IFACE);
  blkidx_t *blockIndex = GetBlockTable(OMNI_COIN_IFACE);
  uint256 hash = GetHash();
  CBlockIndex *pindexNew;
  shtime_t ts;

  // Check for duplicate
  if (blockIndex->count(hash)) 
    return error(SHERR_INVAL, "AddToBlockIndex() : %s already exists", hash.GetHex().c_str());

  /* create new index */
  pindexNew = new CBlockIndex(*this);
  if (!pindexNew)
    return error(SHERR_INVAL, "AddToBlockIndex() : new CBlockIndex failed");

  map<uint256, CBlockIndex*>::iterator mi = blockIndex->insert(make_pair(hash, pindexNew)).first;
  pindexNew->phashBlock = &((*mi).first);
  map<uint256, CBlockIndex*>::iterator miPrev = blockIndex->find(hashPrevBlock);
  if (miPrev != blockIndex->end())
  {
    pindexNew->pprev = (*miPrev).second;
    pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
  }
  pindexNew->bnChainWork = (pindexNew->pprev ? pindexNew->pprev->bnChainWork : 0) + pindexNew->GetBlockWork();

  if (pindexNew->bnChainWork > bnBestChainWork) {
    OMNITxDB txdb;
    bool ret = SetBestChain(txdb, pindexNew);
    txdb.Close();
    if (!ret)
      return false;
  } else {
    /* usher to the holding cell */
    if (hashPrevBlock == GetBestBlockChain(iface)) {
      if (!WriteBlock(pindexNew->nHeight)) {
        return error(SHERR_INVAL, "OMNI: AddToBLocKindex: WriteBlock failure");
      }
    } else {
      WriteArchBlock();
      Debug("OMNIBlock::AddToBlockIndex: skipping block (%s) SetBestChain() since pindexNew->bnChainWork(%s) <= bnBestChainWork(%s)", pindexNew->GetBlockHash().GetHex().c_str(), pindexNew->bnChainWork.ToString().c_str(), bnBestChainWork.ToString().c_str());
    }
  }

  return true;
}

bool OMNIBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{
  return (core_DisconnectBlock(txdb, pindex, this));
}
