
/*
 * @copyright
 *
 *  Copyright 2016 Neo Natura
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
#include "block.h"
#include "main.h"
#include "wallet.h"
#include "coin_proto.h"
#include "shc/shc_netmsg.h"
#include "shc/shc_block.h"
#include "shc/shc_wallet.h"


static int shc_init(CIface *iface, void *_unused_)
{
int err;

  if (!bitdb.Open(GetDataDir())) /* DEBUG: */
  {
    fprintf(stderr, "error: unable to open data directory.\n");
    return (SHERR_INVAL);
  }

  if (!LoadBlockIndex(iface)) {
    fprintf(stderr, "error: unable to open load block index.\n");
    return (SHERR_INVAL);
  }

  shc_LoadWallet();

  err = unet_bind(SHC_COIN_IFACE, SHC_COIN_DAEMON_PORT);
  if (err)
    return (err);

  unet_timer_set(SHC_COIN_IFACE, shc_server_timer); /* x10/s */
  unet_connop_set(SHC_COIN_IFACE, shc_server_accept);
  unet_disconnop_set(SHC_COIN_IFACE, shc_server_close);

  /* automatically connect to peers of 'shc' service. */
  unet_bind_flag_set(SHC_COIN_IFACE, UNETF_PEER_SCAN);

return (0);
}

static int shc_term(CIface *iface, void *_unused_)
{
}
static int shc_msg_recv(CIface *iface, CNode *pnode)
{

  if (!pnode)
    return (0);

  if (!shc_ProcessMessages(iface, pnode)) {
    /* log */
  }

return (0);
}
static int shc_msg_send(CIface *iface, CNode *pnode)
{

  if (!pnode)
    return (0);

  if (!shc_SendMessages(iface, pnode, false)) {
    /* log */
  }

return (0);
}
static int shc_peer_add(CIface *iface, void *arg)
{
return (0);
}
static int shc_peer_recv(CIface *iface, void *arg)
{
return (0);
}

static int shc_block_new(CIface *iface, CBlock **block_p)
{
  *block_p = new SHCBlock();
  return (0);
}

static int shc_block_templ(CIface *iface, CBlock **block_p)
{
  CWallet *wallet = GetWallet(iface);
  int ifaceIndex = GetCoinIndex(iface);
  CBlock* pblock;
  unsigned int median;
  int reset;
    
  if (!wallet) {
    unet_log(ifaceIndex, "GetBlocKTemplate: Wallet not initialized.");
    return (NULL);
  }

  median = pindexBest->GetMedianTimePast() + 1;

  CReserveKey reservekey(wallet);
  pblock = shc_CreateNewBlock(reservekey);
  if (!pblock)
    return (NULL);

  pblock->nTime = MAX(median, GetAdjustedTime());
  pblock->nNonce = 0;

  *block_p = pblock;

  return (0);
}

static int shc_block_submit(CIface *iface, CBlock *block)
{
  blkidx_t *blockIndex;

  blockIndex = GetBlockTable(SHC_COIN_IFACE);
  if (!blockIndex)
    return (STERR_INVAL);

  // Check for duplicate
  uint256 hash = block->GetHash();
  if (blockIndex->count(hash))// || mapOrphanBlocks.count(hash))
    return (BLKERR_DUPLICATE_BLOCK);

  // Preliminary checks
  if (!block->CheckBlock()) {
    shcoind_log("c_processblock: !CheckBlock()");
    return (BLKERR_CHECKPOINT);
  }

  // Store to disk
  if (!block->AcceptBlock()) {
    shcoind_log("c_processblock: !AcceptBlock()");
    return (BLKERR_INVALID_BLOCK);
  }

  block->print();

return (0);
}

static int shc_block_info(CIface *iface, void *arg)
{
return (0);
}

static int shc_tx_new(CIface *iface, void *arg)
{
return (0);
}

static int shc_tx_info(CIface *iface, void *arg)
{
return (0);
}
static int shc_addr_new(CIface *iface, void *arg)
{
return (0);
}
static int shc_addr_import(CIface *iface, void *arg)
{
return (0);
}
static int shc_addr_info(CIface *iface, void *arg)
{
return (0);
}
static int shc_account_new(CIface *iface, void *arg)
{
return (0);
}
static int shc_account_move(CIface *iface, void *arg)
{
return (0);
}
static int shc_account_info(CIface *iface, void *arg)
{
return (0);
}








#ifdef __cplusplus
extern "C" {
#endif

coin_iface_t shc_coin_iface = {
  "usde",
  COIN_IFACE_VERSION(SHC_VERSION_MAJOR, SHC_VERSION_MINOR,
      SHC_VERSION_REVISION, SHC_VERSION_BUILD),
  1, /* block version */
  SHC_COIN_DAEMON_PORT,
  SHC_MAX_BLOCK_SIZE,
  SHC_MAX_BLOCK_SIZE_GEN,
  SHC_MAX_BLOCK_SIGOPS,
  SHC_MAX_ORPHAN_TRANSACTIONS,
  SHC_MIN_TX_FEE,
  SHC_MIN_RELAY_TX_FEE,
  SHC_MAX_MONEY,
  SHC_COINBASE_MATURITY, 
  SHC_LOCKTIME_THRESHOLD,
  COINF(shc_init),
  COINF(shc_term),
  COINF(shc_msg_recv),
  COINF(shc_msg_send),
  COINF(shc_peer_add),
  COINF(shc_peer_recv),
  COINF(shc_block_new),
  COINF(shc_block_templ),
  COINF(shc_block_submit),
  COINF(shc_block_info),
  COINF(shc_tx_new),
  COINF(shc_tx_info),
  COINF(shc_addr_new),
  COINF(shc_addr_import),
  COINF(shc_addr_info),
  COINF(shc_account_new),
  COINF(shc_account_move),
  COINF(shc_account_info),
};


#ifdef __cplusplus
}
#endif
