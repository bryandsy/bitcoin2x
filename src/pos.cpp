// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pos.h"

bool fStakeRun = false;
int64_t nLastCoinStakeSearchInterval = 0;
unsigned int nModifierInterval = 10 * 60;

#ifdef ENABLE_WALLET
// novacoin: attempt to generate suitable proof-of-stake
bool SignBlock(CBlock& block, CWallet& wallet)
{
    block.nVersion |= VERSIONBITS_POS;
    static int64_t nLastCoinStakeSearchTime = GetAdjustedTime(); // startup timestamp

    CKey key;
    CMutableTransaction txCoinStake;

    int64_t nSearchTime = block.nTime; // search to current time

    if (nSearchTime > nLastCoinStakeSearchTime)
    {
        if (wallet.CreateCoinStake(wallet, block.nBits, block.nTime, txCoinStake, key))
        {
            block.vtx.insert(block.vtx.begin() + 1, MakeTransactionRef(std::move(txCoinStake)));
            CMutableTransaction tx(*block.vtx[0]);
            tx.vout.pop_back();
            block.vtx[0] = MakeTransactionRef(std::move(tx));
            GenerateCoinbaseCommitment(block, chainActive.Tip(), Params().GetConsensus());
            block.hashMerkleRoot = BlockMerkleRoot(block);
            block.prevoutStake = block.vtx[1]->vin[0].prevout;

            return key.Sign(block.GetHash(), block.vchBlockSig);
        }
        nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
        nLastCoinStakeSearchTime = nSearchTime;
    } else {
        LogPrint(BCLog::STAKE, "%s: Already tried to sign within interval, exiting\n", __func__);
    }

    return false;
}
#endif

void ThreadStakeMiner(CWallet *pwallet)
{

    // Make this thread recognisable as the mining thread
    RenameThread("b2x-staker");
    std::shared_ptr<CReserveScript> coinbase_script;
    pwallet->GetScriptForMining(coinbase_script);

    while (fStakeRun)
    {
        while (pwallet->IsLocked()) {
            nLastCoinStakeSearchInterval = 0;
            LogPrint(BCLog::STAKE, "%s: Wallet locked, waitig\n", __func__);
            MilliSleep(10000);
        }

        // while (IsInitialBlockDownload()) {
        //     LogPrint(BCLog::STAKE, "%s: Initial blockchain downloading, waitig\n", __func__);
        //     MilliSleep(10000);
        //     continue;
        // }

        // if (g_connman && (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)) {
        //     LogPrint(BCLog::STAKE, "%s: No connections, waitig\n", __func__);
        //     MilliSleep(10000);
        //     continue;
        // }

        auto pblocktemplate(BlockAssembler(Params()).CreateNewBlock(coinbase_script->reserveScript));
        
        if (!pblocktemplate.get()) {
            LogPrint(BCLog::STAKE, "%s: Failed to create block template, exiting\n", __func__);
            return;
        }
            
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

        if (CreatePoSBlock(pblock, *pwallet)) {
                LOCK(cs_main);
                if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash()) {
                    LogPrint(BCLog::STAKE, "%s: Generated block is stale, starting over\n", __func__);
                }
                // Track how many getdata requests this block gets
                {
                    LOCK(pwallet->cs_wallet);
                    pwallet->mapRequestCount[pblock->GetHash()] = 0;
                }
                // Process this block the same as if we had received it from another node
                if (!ProcessNewBlock(Params(), pblock, true, nullptr)) {
                    LogPrint(BCLog::STAKE, "%s: block not accepted, starting over\n", __func__);
                }
        }
        else {
            LogPrint(BCLog::STAKE, "%s: Failed to create PoS block, waiting\n", __func__);
            MilliSleep(10000);
        }
        MilliSleep(500);
    }
}

bool CreatePoSBlock(std::shared_ptr<CBlock> pblock, CWallet& wallet) {
    if (!SignBlock(*pblock, wallet)) {
        LogPrint(BCLog::STAKE, "%s: failed to sign block\n", __func__);
        return false;
    }
    CBlockIndex* pindexPrev = chainActive.Tip();
    if (!CheckProofOfStake(pcoinsTip, pindexPrev->bnStakeModifierV2, pindexPrev->nHeight, pblock->nBits, pblock->nTime, pblock->vtx[1]->vin[0].prevout)) {
        LogPrint(BCLog::STAKE, "%s: check new PoS failed\n", __func__);
        return false;
    }
    return true;
}

void StakeB2X(bool fStake, CWallet *pwallet)
{
    static boost::thread_group* stakeThread = NULL;

    if (stakeThread != NULL)
    {
        stakeThread->interrupt_all();
        delete stakeThread;
        stakeThread = NULL;
    }

    if(fStake && pwallet)
	{
	    stakeThread = new boost::thread_group();
	    stakeThread->create_thread(boost::bind(&ThreadStakeMiner, pwallet));
	}
    fStakeRun = fStake;
}

using namespace std;

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifierV2(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256();  // genesis block's modifier is 0

    CDataStream ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->bnStakeModifierV2;
    return Hash(ss.begin(), ss.end());
}

bool CheckBlockSignature(const CBlock& block)
{
    if (block.IsProofOfWork())
        return block.vchBlockSig.empty();

    if (block.vchBlockSig.empty())
        return false;

    std::vector<valtype> vSolutions;
    txnouttype whichType;

    const CTxOut& txout = block.vtx[1]->vout[1];

    if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        valtype& vchPubKey = vSolutions[0];
        return CPubKey(vchPubKey).Verify(block.GetHash(), block.vchBlockSig);
    }
    else
    {
        // Block signing key also can be encoded in the nonspendable output
        // This allows to not pollute UTXO set with useless outputs e.g. in case of multisig staking

        const CScript& script = txout.scriptPubKey;
        CScript::const_iterator pc = script.begin();
        opcodetype opcode;
        valtype vchPushValue;

        if (!script.GetOp(pc, opcode, vchPushValue))
            return false;
        if (opcode != OP_RETURN)
            return false;
        if (!script.GetOp(pc, opcode, vchPushValue))
            return false;
        if (!IsCompressedOrUncompressedPubKey(vchPushValue))
            return false;
        return CPubKey(vchPushValue).Verify(block.GetHash(), block.vchBlockSig);
    }

    return false;
}

// BlackCoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage,
//                       obsolete since v3
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(uint256 bnStakeModifierV2, int nPrevHeight, uint32_t nBits, uint32_t nTime, uint32_t nTimeBlockFrom, const COutPoint& prevout, const CTxOut& txout)
{
    uint32_t nStakeTime = nTimeBlockFrom & ~STAKE_TIMESTAMP_MASK;

    if (nTime < nStakeTime) {
        LogPrint(BCLog::STAKE, "%s: nTime violation\n", __func__);
        return false;
    }

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = txout.nValue;
    arith_uint256 bnWeight = arith_uint256(nValueIn);
    bnTarget *= bnWeight;

    int64_t nStakeModifierTime = nStakeTime;

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << bnStakeModifierV2;
    ss << nStakeModifierTime << prevout.hash << prevout.n << nTime;
    uint256 hashProofOfStake = Hash(ss.begin(), ss.end());

    LogPrint(BCLog::STAKE, "CheckStakeKernelHash() : using block at height=%d timestamp=%s for block from timestamp=%s\n",
        nPrevHeight,
        DateTimeStrFormat(nStakeModifierTime),
        DateTimeStrFormat(nTimeBlockFrom));
    LogPrint(BCLog::STAKE, "CheckStakeKernelHash() : check nTimeBlockFrom=%u nStakeTime=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
        nTimeBlockFrom, nStakeModifierTime, prevout.n, nTime,
        hashProofOfStake.ToString());

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnTarget)
        return error("CheckStakeKernelHash() : target not met %s < %s\n", bnTarget.ToString(), hashProofOfStake.ToString());

    LogPrint(BCLog::STAKE, "CheckStakeKernelHash() : using block at height=%d timestamp=%s for block from timestamp=%s\n",
        nPrevHeight,
        DateTimeStrFormat(nStakeModifierTime),
        DateTimeStrFormat(nTimeBlockFrom));
    LogPrint(BCLog::STAKE, "CheckStakeKernelHash() : pass nTimeBlockFrom=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
        nTimeBlockFrom, prevout.n, nTime,
        hashProofOfStake.ToString());


    return true;
}

/*
// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, CValidationState& state, const CTransaction& tx, unsigned int nTime, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTransaction txPrev;
    CDiskTxPos txindex;
    if (!ReadFromDisk(txPrev, txindex, *pblocktree, txin.prevout))
        return state.DoS(1, error("CheckProofOfStake() : INFO: read txPrev failed"));  // previous transaction not in main chain, may occur during initial download

    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, SCRIPT_VERIFY_NONE))
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));

    // Read block header
    CBlockHeader block;
    if (!ReadFromDisk(block, txindex.nFile, txindex.nPos))
        return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction

    // Min age requirement
    int nDepth;
    if (IsConfirmedInNPrevBlocks(txindex, pindexPrev, nStakeMinConfirmations - 1, nDepth))
        return state.DoS(100, error("CheckProofOfStake() : tried to stake at depth %d", nDepth + 1));

    if (!CheckStakeKernelHash(pindexPrev, nBits, block, txindex.nTxOffset - txindex.nPos, txPrev, txin.prevout, nTime, hashProofOfStake, targetProofOfStake, fDebug))
        return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}
*/

bool CheckProofOfStake(CCoinsViewCache* view, uint256 bnStakeModifierV2, int nPrevHeight, uint32_t nBits, uint32_t nTime, const COutPoint& prevout) {
    if (!view->HaveCoin(prevout)) {
        LogPrint(BCLog::STAKE, "%s: inputs missing/spent\n", __func__);
        return false;
    }

    auto coin = view->AccessCoin(prevout);

    if (nPrevHeight + 1 - coin.nHeight < COINBASE_MATURITY) {
        LogPrint(BCLog::STAKE, "%s: tried to stake at depth %d\n", __func__, nPrevHeight + 1 - coin.nHeight);
        return false;
    }

    auto prevTime = chainActive[coin.nHeight]->nTime;

    if (nTime - prevTime < STAKE_MIN_AGE) {
        LogPrint(BCLog::STAKE, "%s: tried to stake at age %d\n", __func__, nTime - prevTime);
        return false;
    }
    if (!CheckStakeKernelHash(bnStakeModifierV2, nPrevHeight, nBits, nTime, prevTime, prevout, coin.out)) {
        LogPrint(BCLog::STAKE, "%s: check kernel failed on coinstake %s\n", __func__, prevout.hash.ToString());
        return false;
    }
    return true;
}