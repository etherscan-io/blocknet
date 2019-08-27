// Copyright (c) 2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BLOCKNET_GOVERNANCE_H
#define BLOCKNET_GOVERNANCE_H

#include <amount.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <key_io.h>
#include <net.h>
#include <policy/policy.h>
#include <script/standard.h>
#include <shutdown.h>
#include <streams.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/moneystr.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/wallet.h>

#include <regex>
#include <string>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>

/**
 * Governance namespace.
 */
namespace gov {

/**
 * Governance types are used with OP_RETURN to indicate how the messages should be processed.
 */
enum Type : uint8_t {
    NONE         = 0,
    PROPOSAL     = 1,
    VOTE         = 2,
};

static const uint8_t NETWORK_VERSION = 0x01;
static const CAmount VOTING_UTXO_INPUT_AMOUNT = 0.1 * COIN;

/**
 * Return the CKeyID for the specified utxo.
 * @param utxo
 * @param keyid
 * @return
 */
static bool GetKeyIDForUTXO(const COutPoint & utxo, CTransactionRef & tx, CKeyID & keyid) {
    uint256 hashBlock;
    if (!GetTransaction(utxo.hash, tx, Params().GetConsensus(), hashBlock))
        return false;
    if (utxo.n >= tx->vout.size())
        return false;
    CTxDestination dest;
    if (!ExtractDestination(tx->vout[utxo.n].scriptPubKey, dest))
        return false;
    keyid = *boost::get<CKeyID>(&dest);
    return true;
}

/**
 * Check that utxo isn't already spent
 * @param utxo
 * @return
 */
static bool IsUTXOSpent(const COutPoint & utxo, const bool & mempoolCheck = true) {
    Coin coin;
    if (mempoolCheck) {
        LOCK2(cs_main, mempool.cs);
        CCoinsViewMemPool view(pcoinsTip.get(), mempool);
        if (!view.GetCoin(utxo, coin) || mempool.isSpent(utxo))
            return true;
    } else {
        LOCK(cs_main);
        if (!pcoinsTip->GetCoin(utxo, coin))
            return true;
    }
    return false;
}

/**
 * Returns the next superblock from the most recent chain tip.
 * @param params
 * @return
 */
static int NextSuperblock(const Consensus::Params & params, const int fromBlock = 0) {
    if (fromBlock == 0) {
        LOCK(cs_main);
        return chainActive.Height() - chainActive.Height() % params.superblock + params.superblock;
    }
    return fromBlock - fromBlock % params.superblock + params.superblock;
}

/**
 * Returns the previous superblock from the most recent chain tip.
 * @param params
 * @param fromBlock
 * @return
 */
static int PreviousSuperblock(const Consensus::Params & params, const int fromBlock = 0) {
    const int nextSuperblock = NextSuperblock(params, fromBlock);
    return nextSuperblock - params.superblock;
}

/**
 * Encapsulates serialized OP_RETURN governance data.
 */
class NetworkObject {
public:
    explicit NetworkObject() = default;

    /**
     * Returns true if this network data contains the proper version.
     * @return
     */
    bool isValid() const {
        return version == NETWORK_VERSION;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(version);
        READWRITE(type);
    }

    const uint8_t & getType() const {
        return type;
    }

protected:
    uint8_t version{NETWORK_VERSION};
    uint8_t type{NONE};
};

/**
 * Proposals encapsulate the data required by the network to support voting and payments.
 * They can be created by anyone willing to pay the submission fee.
 */
class Proposal {
public:
    explicit Proposal(std::string name, int superblock, CAmount amount, std::string address,
                      std::string url, std::string description) : name(std::move(name)), superblock(superblock),
                                              amount(amount), address(std::move(address)), url(std::move(url)),
                                              description(std::move(description)) {}
    explicit Proposal(int blockNumber) : blockNumber(blockNumber) {}
    Proposal() = default;
    Proposal(const Proposal &) = default;
    Proposal& operator=(const Proposal &) = default;
    friend inline bool operator==(const Proposal & a, const Proposal & b) { return a.getHash() == b.getHash(); }
    friend inline bool operator!=(const Proposal & a, const Proposal & b) { return !(a.getHash() == b.getHash()); }
    friend inline bool operator<(const Proposal & a, const Proposal & b) { return a.getHash() < b.getHash(); }

    /**
     * Null check
     * @return
     */
    bool isNull() const {
        return superblock == 0;
    }

    /**
     * Valid if the proposal properties are correct.
     * @param params
     * @param failureReasonRet
     * @return
     */
    bool isValid(const Consensus::Params & params, std::string *failureReasonRet=nullptr) const {
        static std::regex rrname("^\\w+[\\w- ]*\\w+$");
        if (!std::regex_match(name, rrname)) {
            if (failureReasonRet) *failureReasonRet = strprintf("Proposal name %s is invalid, only alpha-numeric characters are accepted", name);
            return false;
        }
        if (superblock % params.superblock != 0) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad superblock number, did you mean %d", gov::NextSuperblock(params));
            return false;
        }
        if (!(amount >= params.proposalMinAmount && amount <= params.GetBlockSubsidy(superblock, params))) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad proposal amount, specify amount between %s - %s", FormatMoney(params.proposalMinAmount), FormatMoney(params.proposalMaxAmount));
            return false;
        }
        if (!IsValidDestination(DecodeDestination(address))) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad payment address %s", address);
            return false;
        }
        if (type != PROPOSAL) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad proposal type, expected %d", PROPOSAL);
            return false;
        }
        if (version != NETWORK_VERSION) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad proposal network version, expected %d", NETWORK_VERSION);
            return false;
        }
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << version << type << name << superblock << amount << address << url << description;
        const int maxBytes = MAX_OP_RETURN_RELAY-3; // -1 for OP_RETURN -2 for pushdata opcodes
        if (ss.size() > maxBytes) {
            if (failureReasonRet) *failureReasonRet = strprintf("Proposal data is too long, try reducing the description by %d characters, expected total of %d bytes, received %d", ss.size()-maxBytes, maxBytes, ss.size());
            return false;
        }
        return true;
    }

    /**
     * Proposal name
     * @return
     */
    const std::string & getName() const {
        return name;
    }

    /**
     * Proposal superblock
     * @return
     */
    const int & getSuperblock() const {
        return superblock;
    }

    /**
     * Proposal amount
     * @return
     */
    const CAmount & getAmount() const {
        return amount;
    }

    /**
     * Proposal address
     * @return
     */
    const std::string & getAddress() const {
        return address;
    }

    /**
     * Proposal url (for more information)
     * @return
     */
    const std::string & getUrl() const {
        return url;
    }

    /**
     * Proposal description
     * @return
     */
    const std::string & getDescription() const {
        return description;
    }

    /**
     * Proposal block number
     * @return
     */
    const int & getBlockNumber() const {
        return blockNumber;
    }

    /**
     * Proposal hash
     * @return
     */
    uint256 getHash() const {
        CHashWriter ss(SER_GETHASH, 0);
        ss << version << type << name << superblock << amount << address << url << description;
        return ss.GetHash();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(version);
        READWRITE(type);
        READWRITE(superblock);
        READWRITE(amount);
        READWRITE(address);
        READWRITE(name);
        READWRITE(url);
        READWRITE(description);
    }

protected:
    uint8_t version{NETWORK_VERSION};
    uint8_t type{PROPOSAL};
    std::string name;
    int superblock{0};
    CAmount amount{0};
    std::string address;
    std::string url;
    std::string description;

protected: // memory only
    int blockNumber{0}; // block containing this proposal
};

enum VoteType : uint8_t {
    NO      = 0,
    YES     = 1,
    ABSTAIN = 2,
};

/**
 * Votes can be cast on proposals and ultimately lead to unlocking funds for proposals that meet
 * the minimum requirements and minimum required votes.
 */
class Vote {
public:
    explicit Vote(const uint256 & proposal, const VoteType & vote, const COutPoint & utxo) : proposal(proposal),
                                                                                             vote(vote),
                                                                                             utxo(utxo) {
        loadKeyID();
    }
    explicit Vote(const COutPoint & outpoint, const int64_t & time = 0, const int & blockNumber = 0) : outpoint(outpoint),
                                                                                                       time(time),
                                                                                                       blockNumber(blockNumber) {}
    Vote() = default;
    Vote(const Vote &) = default;
    Vote& operator=(const Vote &) = default;
    friend inline bool operator==(const Vote & a, const Vote & b) { return a.getHash() == b.getHash(); }
    friend inline bool operator!=(const Vote & a, const Vote & b) { return !(a.getHash() == b.getHash()); }
    friend inline bool operator<(const Vote & a, const Vote & b) { return a.getHash() < b.getHash(); }

    /**
     * Returns true if a valid vote string type was converted.
     * @param strVote
     * @param voteType Mutated with the converted vote type.
     * @return
     */
    static bool voteTypeForString(std::string strVote, VoteType & voteType) {
        boost::to_lower(strVote, std::locale::classic());
        if (strVote == "yes") {
            voteType = YES;
        } else if (strVote == "no") {
            voteType = NO;
        } else if (strVote == "abstain") {
            voteType = ABSTAIN;
        } else {
            return false;
        }
        return true;
    }

    /**
     * Returns the string representation of the vote type.
     * @param voteType
     * @param valid true if conversion was successful, otherwise false.
     * @return
     */
    static std::string voteTypeToString(const VoteType & voteType, bool *valid = nullptr) {
        std::string strVote;
        if (voteType == YES) {
            strVote = "yes";
        } else if (voteType == NO) {
            strVote = "no";
        } else if (voteType == ABSTAIN) {
            strVote = "abstain";
        } else {
            if (valid) *valid = false;
        }
        if (valid) *valid = true;
        return strVote;
    }

    /**
     * Null check
     * @return
     */
    bool isNull() {
        return utxo.IsNull();
    }

    /**
     * Returns true if the vote properties are valid and the utxo pubkey
     * matches the pubkey of the signature.
     * @return
     */
    bool isValid(const Consensus::Params & params) const {
        if (!(version == NETWORK_VERSION && type == VOTE && isValidVoteType(vote)))
            return false;
        if (amount < params.voteMinUtxoAmount) // n bounds checked in GetKeyIDForUTXO
            return false;
        // Ensure the pubkey of the utxo matches the pubkey of the vote signature
        if (keyid.IsNull())
            return false;
        if (pubkey.GetID() != keyid)
            return false;
        if (IsUTXOSpent(utxo))
            return false;
        return true;
    }

    /**
     * Sign the vote with the specified private key.
     * @param key
     * @return
     */
    bool sign(const CKey & key) {
        signature.clear();
        if (!key.SignCompact(sigHash(), signature))
            return false;
        return pubkey.RecoverCompact(sigHash(), signature);
    }

    /**
     * Proposal hash
     * @return
     */
    const uint256 & getProposal() const {
        return proposal;
    }

    /**
     * Proposal vote
     * @return
     */
    VoteType getVote() const {
        return static_cast<VoteType>(vote);
    }

    /**
     * Proposal vote
     * @return
     */
    const std::vector<unsigned char> & getSignature() const {
        return signature;
    }

    /**
     * Proposal utxo containing the vote
     * @return
     */
    const COutPoint & getUtxo() const {
        return utxo;
    }

    /**
     * Proposal hash
     * @return
     */
    uint256 getHash() const {
        CHashWriter ss(SER_GETHASH, 0);
        ss << version << type << proposal << utxo; // exclude vote from hash to properly handle changing votes
        return ss.GetHash();
    }

    /**
     * Proposal signature hash
     * @return
     */
    uint256 sigHash() const {
        CHashWriter ss(SER_GETHASH, 0);
        ss << version << type << proposal << vote << utxo;
        return ss.GetHash();
    }

    /**
     * Get the pubkey associated with the vote's signature.
     * @return
     */
    const CPubKey & getPubKey() const {
        return pubkey;
    }

    /**
     * Get the COutPoint of the vote.
     * @return
     */
    const COutPoint & getOutpoint() const {
        return outpoint;
    }

    /**
     * Get the time of the vote.
     * @return
     */
    const int64_t & getTime() const {
        return time;
    }

    /**
     * Get the amount associated with the vote.
     * @return
     */
    const CAmount & getAmount() const {
        return amount;
    }

    /**
     * Vote block number
     * @return
     */
    const int & getBlockNumber() const {
        return blockNumber;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(version);
        READWRITE(type);
        READWRITE(proposal);
        READWRITE(vote);
        READWRITE(utxo);
        READWRITE(signature);
        if (ser_action.ForRead()) { // assign memory only fields
            pubkey.RecoverCompact(sigHash(), signature);
            loadKeyID();
        }
    }

protected:
    /**
     * Returns true if the unsigned char is a valid vote type enum.
     * @param voteType
     * @return
     */
    bool isValidVoteType(const uint8_t & voteType) const {
        return voteType >= NO && voteType <= ABSTAIN;
    }

    /**
     * Load the keyid and amount.
     */
    void loadKeyID() {
        CTransactionRef tx;
        if (GetKeyIDForUTXO(utxo, tx, keyid))
            amount = tx->vout[utxo.n].nValue;
    }

protected:
    uint8_t version{NETWORK_VERSION};
    uint8_t type{VOTE};
    uint256 proposal;
    uint8_t vote{ABSTAIN};
    std::vector<unsigned char> signature;
    COutPoint utxo; // voting on behalf of this utxo

protected: // memory only
    CPubKey pubkey;
    COutPoint outpoint; // of vote's OP_RETURN outpoint
    int64_t time{0}; // block time of vote
    CAmount amount{0}; // of vote's utxo (this is not the OP_RETURN outpoint amount, which is 0)
    CKeyID keyid; // CKeyID of vote's utxo
    int blockNumber{0}; // block containing this vote
};

/**
 * ProposalVote associates a proposal with a specific vote.
 */
struct ProposalVote {
    Proposal proposal;
    VoteType vote;
};
/**
 * Way to obtain all votes for a specific proposal
 */
struct Tally {
    Tally() = default;
    CAmount cyes{0};
    CAmount cno{0};
    CAmount cabstain{0};
    int yes{0};
    int no{0};
    int abstain{0};
};

/**
 * Manages related servicenode functions including handling network messages and storing an active list
 * of valid servicenodes.
 */
class Governance : public CValidationInterface {
public:
    explicit Governance() = default;

    /**
     * Returns true if the proposal with the specified hash exists.
     * @param hash
     * @return
     */
    bool hasProposal(const uint256 & hash) {
        LOCK(mu);
        return proposals.count(hash) > 0;
    }

    /**
     * Returns true if the vote with the specified hash exists.
     * @param hash
     * @return
     */
    bool hasVote(const uint256 & hash) {
        LOCK(mu);
        return votes.count(hash) > 0;
    }

    /**
     * Returns true if the specified proposal and utxo matches a known vote.
     * @param utxo
     * @return
     */
    bool hasVote(const uint256 & proposal, const COutPoint & utxo) {
        LOCK(mu);
        for (const auto & item : votes) {
            const auto & vote = item.second;
            if (vote.getUtxo() == utxo && vote.getProposal() == proposal)
                return true;
        }
        return false;
    }

    /**
     * Resets the governance state.
     * @return
     */
    bool reset() {
        LOCK(mu);
        proposals.clear();
        votes.clear();
        return true;
    }

    /**
     * Loads the governance data from the blockchain ledger. It's possible to optimize
     * this further by creating a separate leveldb for goverance data. Currently, this
     * method will read every block on the chain and search for goverance data.
     * @return
     */
    bool loadGovernanceData(const CChain & chain, CCriticalSection & chainMutex,
                            const Consensus::Params & consensus, std::string & failReasonRet)
    {
        int blockHeight{0};
        {
            LOCK(chainMutex);
            blockHeight = chain.Height();
        }
        // No need to load any governance data if we on the genesis block
        // or if the governance system hasn't been enabled yet.
        if (blockHeight == 0 || blockHeight < consensus.governanceBlock)
            return true;

        // Shard the blocks into num_cores slices
        boost::thread_group tg;
        const auto cores = GetNumCores();
        Mutex mut; // manage access to shared data

        const int totalBlocks = blockHeight - consensus.governanceBlock;
        int slice = totalBlocks / cores;
        bool failed{false};
        for (int k = 0; k < cores; ++k) {
            const int start = consensus.governanceBlock + k*slice;
            const int end = k == cores-1 ? blockHeight+1 // check bounds, +1 due to "<" logic below, ensure inclusion of last block
                                         : start+slice;
            tg.create_thread([start,end,&failed,&failReasonRet,&chain,&chainMutex,&mut,this] {
                RenameThread("bitcoin-governance");
                for (int blockNumber = start; blockNumber < end; ++blockNumber) {
                    if (ShutdownRequested()) { // don't hold up shutdown requests
                        failed = true;
                        break;
                    }

                    CBlockIndex *blockIndex;
                    {
                        LOCK(chainMutex);
                        blockIndex = chain[blockNumber];
                    }
                    if (!blockIndex) {
                        LOCK(mut);
                        failed = true;
                        failReasonRet += strprintf("Failed to read block index for block %d\n", blockNumber);
                        return;
                    }

                    CBlock block;
                    if (!ReadBlockFromDisk(block, blockIndex, Params().GetConsensus())) {
                        LOCK(mut);
                        failed = true;
                        failReasonRet += strprintf("Failed to read block from disk for block %d\n", blockNumber);
                        return;
                    }
                    // Process block
                    const auto sblock = std::make_shared<const CBlock>(block);
                    BlockConnected(sblock, blockIndex, {});
                }
            });
        }
        // Wait for all threads to complete
        tg.join_all();

        {
            LOCK(mu);
            if (votes.empty() || failed)
                return !failed;
        }

        // Now that all votes are loaded, check and remove any invalid ones.
        // Invalid votes can be evaluated using multiple threads since we
        // have the complete dataset in memory. Below the votes are sliced
        // up into shards and each available thread works on its own shard.
        std::vector<std::pair<uint256, Vote>> tmpvotes;
        tmpvotes.reserve(votes.size());
        {
            LOCK(mu);
            std::copy(votes.begin(), votes.end(), std::back_inserter(tmpvotes));
        }
        slice = static_cast<int>(tmpvotes.size()) / cores;
        for (int k = 0; k < cores; ++k) {
            const int start = k*slice;
            const int end = k == cores-1 ? static_cast<int>(tmpvotes.size())
                                         : start+slice;
            try {
                tg.create_thread([start,end,&tmpvotes,&failed,&mut,this] {
                    RenameThread("bitcoin-governance");
                    for (int i = start; i < end; ++i) {
                        if (ShutdownRequested()) { // don't hold up shutdown requests
                            failed = true;
                            break;
                        }
                        Vote vote;
                        {
                            LOCK(mut);
                            vote = tmpvotes[i].second;
                        }
                        // Erase votes with spent utxos
                        if (IsUTXOSpent(vote.getUtxo(), false)) { // no mempool check required here (might not be loaded anyways)
                            LOCK(mu);
                            votes.erase(vote.getHash());
                        }
                    }
                });
            } catch (std::exception & e) {
                failed = true;
                failReasonRet += strprintf("Failed to create thread to load governance data: %s\n", e.what());
            }
        }
        // Wait for all threads to complete
        tg.join_all();

        return !failed;
    }

    /**
     * Fetch the specified proposal.
     * @param hash Proposal hash
     * @return
     */
    Proposal getProposal(const uint256 & hash) {
        LOCK(mu);
        if (proposals.count(hash) > 0)
            return proposals[hash];
        return Proposal{};
    }

    /**
     * Fetch the specified vote.
     * @param hash Vote hash
     * @return
     */
    Vote getVote(const uint256 & hash) {
        LOCK(mu);
        if (votes.count(hash) > 0)
            return votes[hash];
        return Vote{};
    }

    /**
     * Fetch the list of all known proposals.
     * @return
     */
    std::vector<Proposal> getProposals() {
        LOCK(mu);
        std::vector<Proposal> props;
        props.reserve(proposals.size());
        for (const auto & item : proposals)
            props.push_back(item.second);
        return std::move(props);
    }

    /**
     * Fetch the list of all known votes.
     * @return
     */
    std::vector<Vote> getVotes() {
        LOCK(mu);
        std::vector<Vote> vos;
        vos.reserve(votes.size());
        for (const auto & item : votes)
            vos.push_back(item.second);
        return std::move(vos);
    }

    /**
     * Fetch all votes for the specified proposal.
     * @param hash Proposal hash
     * @return
     */
    std::vector<Vote> getVotes(const uint256 & hash) {
        LOCK(mu);
        std::vector<Vote> vos;
        for (const auto & item : votes) {
            if (item.second.getProposal() == hash)
                vos.push_back(item.second);
        }
        return std::move(vos);
    }

public: // static

    /**
     * Singleton instance.
     * @return
     */
    static Governance & instance() {
        static Governance gov;
        return gov;
    }

    /**
     * Returns the upcoming superblock.
     * @param params
     * @return
     */
    static int nextSuperblock(const Consensus::Params & params) {
        return NextSuperblock(params);
    }

    /**
     * Returns the superblock immediately after the specified block.
     * @param fromBlock
     * @param params
     * @return
     */
    static int nextSuperblock(const int fromBlock, const Consensus::Params & params) {
        return NextSuperblock(params, fromBlock);
    }

    /**
     * Returns true if the proposal meets the requirements for the cutoff.
     * @param proposal
     * @param blockNumber
     * @param params
     * @return
     */
    static bool meetsCutoff(const Proposal & proposal, const int & blockNumber, const Consensus::Params & params) {
        // Proposals can be submitted multiple superblocks in advance. As a
        // result, a proposal meets the cutoff for a block number that's prior
        // to the proposal's superblock.
        return blockNumber <= proposal.getSuperblock() - Params().GetConsensus().proposalCutoff;
    }

    /**
     * Returns true if the vote meets the requirements for the cutoff. Make sure mutex (mu) is not held.
     * @param vote
     * @param blockNumber
     * @param params
     * @return
     */
    static bool meetsCutoff(const Vote & vote, const int & blockNumber, const Consensus::Params & params) {
        const auto & proposal = instance().getProposal(vote.getProposal());
        if (proposal.isNull()) // if no proposal found
            return false;
        // Votes can happen multiple superblocks in advance if a proposal is
        // created for a future superblock. As a result, a vote meets the
        // cutoff for a block number that's prior to the superblock of its
        // associated proposal.
        return blockNumber <= proposal.getSuperblock() - Params().GetConsensus().votingCutoff;
    }

    /**
     * If the vote's pubkey matches the specified vin's pubkey returns true, otherwise
     * returns false.
     * @param vote
     * @param vin
     * @return
     */
    static bool matchesVinPubKey(const Vote & vote, const CTxIn & vin) {
        CScript::const_iterator pc = vin.scriptSig.begin();
        std::vector<unsigned char> data;
        bool isPubkey{false};
        while (pc < vin.scriptSig.end()) {
            opcodetype opcode;
            if (!vin.scriptSig.GetOp(pc, opcode, data))
                break;
            if (data.size() == CPubKey::PUBLIC_KEY_SIZE || data.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE) {
                isPubkey = true;
                break;
            }
        }

        if (!isPubkey)
            return false; // skip, no match

        CPubKey pubkey(data);
        return pubkey.GetID() == vote.getPubKey().GetID();
    }

    /**
     * Obtains all votes and proposals from the specified block.
     * @param block
     * @param blockIndex
     * @param proposalsRet
     * @param votesRet
     * @return
     */
    static void dataFromBlock(const CBlock *block, std::set<Proposal> & proposalsRet, std::set<Vote> & votesRet, const CBlockIndex *blockIndex=nullptr) {
        const auto & consensus = Params().GetConsensus();
        for (const auto & tx : block->vtx) {
            if (tx->IsCoinBase())
                continue;
            for (int n = 0; n < static_cast<int>(tx->vout.size()); ++n) {
                const auto & out = tx->vout[n];
                if (out.scriptPubKey[0] != OP_RETURN)
                    continue; // no proposal data
                CScript::const_iterator pc = out.scriptPubKey.begin();
                std::vector<unsigned char> data;
                while (pc < out.scriptPubKey.end()) {
                    opcodetype opcode;
                    if (!out.scriptPubKey.GetOp(pc, opcode, data))
                        break;
                    if (!data.empty())
                        break;
                }

                CDataStream ss(data, SER_NETWORK, PROTOCOL_VERSION);
                NetworkObject obj; ss >> obj;
                if (!obj.isValid())
                    continue; // must match expected version

                if (obj.getType() == PROPOSAL) {
                    CDataStream ss2(data, SER_NETWORK, PROTOCOL_VERSION);
                    Proposal proposal(blockIndex ? blockIndex->nHeight : 0); ss2 >> proposal;
                    // Skip the cutoff check if block index is not specified
                    if (proposal.isValid(consensus) && (!blockIndex || meetsCutoff(proposal, blockIndex->nHeight, consensus)))
                        proposalsRet.insert(proposal);
                } else if (obj.getType() == VOTE) {
                    CDataStream ss2(data, SER_NETWORK, PROTOCOL_VERSION);
                    Vote vote({tx->GetHash(), static_cast<uint32_t>(n)}, block->GetBlockTime(), blockIndex ? blockIndex->nHeight : 0);
                    ss2 >> vote;
                    // Check that the vote is valid and that it meets the cutoff requirements
                    if (!vote.isValid(consensus) || (blockIndex && !meetsCutoff(vote, blockIndex->nHeight, consensus)))
                        continue;
                    // Check to make sure that a valid signature exists in the vin scriptSig
                    // that matches the same pubkey used in the vote signature.
                    bool validVin{false};
                    for (const auto & vin : tx->vin) {
                        if (matchesVinPubKey(vote, vin)) {
                            validVin = true;
                            break;
                        }
                    }
                    // if the vote is properly associated with a vin
                    if (validVin) {
                        // Handle vote changes, if a vote already exists and the user
                        // is submitting a change, only count the vote with the most
                        // recent timestamp. If a vote on the same utxo occurs in the
                        // same block, the vote with the larger hash is chosen as the
                        // tie breaker. This could have unintended consequences if the
                        // user intends the smaller hash to be the most recent vote.
                        // The best way to handle this is to build the voting client
                        // to require waiting at least 1 block between vote changes.
                        // Changes to this logic below must also be applied to "BlockConnected()"
                        if (votesRet.count(vote)) {
                            // Assumed that all votes in the same block have the same "time"
                            auto it = votesRet.find(vote);
                            if (UintToArith256(vote.sigHash()) > UintToArith256(it->sigHash()))
                                votesRet.insert(std::move(vote));
                        } else // if no vote exists then add
                            votesRet.insert(std::move(vote));
                    }
                }
            }
        }
    }

    /**
     * Returns the vote tally for the specified proposal.
     * @param proposal
     * @param votes Vote array to search
     * @param params
     * @return
     */
    static Tally getTally(const uint256 & proposal, const std::vector<Vote> & votes, const Consensus::Params & params) {
        // Organize votes by tx hash to designate common votes (from same user)
        // We can assume all the votes in the same tx are associated with the
        // same user (i.e. all privkeys in the votes are known by the tx signer)
        std::map<uint256, std::set<Vote>> userVotes;
        // Cross reference all votes associated with a destination. If a vote
        // is associated with a common destination we can assume the same user
        // casted the vote. All votes in the tx imply the same user and all
        // votes associated with the same destination imply the same user.
        std::map<CTxDestination, std::set<Vote>> userVotesDest;

        std::vector<Vote> proposalVotes = votes;
        // remove all votes that don't match the proposal
        proposalVotes.erase(std::remove_if(proposalVotes.begin(), proposalVotes.end(), [&proposal](const Vote & vote) -> bool {
            return proposal != vote.getProposal();
        }), proposalVotes.end());

        // Prep our search containers
        for (const auto & vote : proposalVotes) {
            std::set<Vote> & v = userVotes[vote.getOutpoint().hash];
            v.insert(vote);
            userVotes[vote.getOutpoint().hash] = v;

            std::set<Vote> & v2 = userVotesDest[CTxDestination{vote.getPubKey().GetID()}];
            v2.insert(vote);
            userVotesDest[CTxDestination{vote.getPubKey().GetID()}] = v2;
        }

        // Iterate over all transactions and associated votes. In order to
        // prevent counting too many votes we need to tally up votes
        // across users separately and only count up their respective
        // votes in lieu of the maximum vote balance requirements.
        std::set<Vote> counted; // track counted votes
        std::vector<Tally> tallies;
        for (const auto & item : userVotes) {
            // First count all unique votes associated with the same tx.
            // This indicates they're all likely from the same user or
            // group of users pooling votes.
            std::set<Vote> allUnique;
            allUnique.insert(item.second.begin(), item.second.end());
            for (const auto & vote : item.second) {
                // Add all unique votes associated with the same destination.
                // Since we're first iterating over all the votes in the
                // same tx, and then over the votes based on common destination
                // we're able to get all the votes associated with a user.
                // The only exception is if a user votes from different wallets
                // and doesn't reveal the connection by combining into the same
                // tx. As a result, there's an optimal way to cast votes and that
                // should be taken into consideration on the voting client.
                const auto & destVotes = userVotesDest[CTxDestination{vote.getPubKey().GetID()}];
                allUnique.insert(destVotes.begin(), destVotes.end());
            }

            // Prevent counting votes more than once
            for (auto it = allUnique.begin(); it != allUnique.end(); ) {
                if (counted.count(*it) > 0)
                    allUnique.erase(it++);
                else ++it;
            }

            if (allUnique.empty())
                continue; // nothing to count
            counted.insert(allUnique.begin(), allUnique.end());

            Tally tally;
            for (const auto & vote : allUnique)  {
                if (vote.getVote() == gov::YES)
                    tally.cyes += vote.getAmount();
                else if (vote.getVote() == gov::NO)
                    tally.cno += vote.getAmount();
                else if (vote.getVote() == gov::ABSTAIN)
                    tally.cabstain += vote.getAmount();
            }
            tally.yes = static_cast<int>(tally.cyes / params.voteBalance);
            tally.no = static_cast<int>(tally.cno / params.voteBalance);
            tally.abstain = static_cast<int>(tally.cabstain / params.voteBalance);
            tallies.push_back(tally);
        }

        // Tally all votes across all users that voted on this proposal
        Tally finalTally;
        for (const auto & tally : tallies) {
            finalTally.yes += tally.yes;
            finalTally.no += tally.no;
            finalTally.abstain += tally.abstain;
            finalTally.cyes += tally.cyes;
            finalTally.cno += tally.cno;
            finalTally.cabstain += tally.cabstain;
        }
        return finalTally;
    }

    /**
     * Cast votes on proposals.
     * @param proposals
     * @param params
     * @param txsRet List of transactions containing proposal votes
     * @param failReason Error message (empty if no error)
     * @return
     */
    static bool submitVotes(const std::vector<ProposalVote> & proposals, const std::vector<std::shared_ptr<CWallet>> & vwallets,
                            const Consensus::Params & params, std::vector<CTransactionRef> & txsRet, std::string *failReasonRet=nullptr)
    {
        if (proposals.empty())
            return false; // no proposals specified, reject

        for (const auto & pv : proposals) { // check if any proposals are invalid
            if (!pv.proposal.isValid(params)) {
                *failReasonRet = strprintf("Failed to vote on proposal (%s) because it's invalid", pv.proposal.getName());
                return error(failReasonRet->c_str());
            }
        }

        txsRet.clear(); // prep tx result
        CAmount totalBalance{0};
        std::vector<std::shared_ptr<CWallet>> wallets = vwallets;
        if (wallets.empty())
            wallets = GetWallets();

        // Make sure wallets are available
        if (wallets.empty()) {
            *failReasonRet = "No wallets were found";
            return error(failReasonRet->c_str());
        }

        // Make sure there's enough coin to cast a vote
        for (auto & wallet : wallets) {
            if (wallet->IsLocked()) {
                *failReasonRet = "All wallets must be unlocked to vote";
                return error(failReasonRet->c_str());
            }
            totalBalance += wallet->GetBalance();
        }
        if (totalBalance < params.voteBalance) {
            *failReasonRet = strprintf("Not enough coin to cast a vote, %s is required", FormatMoney(params.voteBalance));
            return error(failReasonRet->c_str());
        }

        // Create the transactions that will required to casts votes
        // An OP_RETURN is required for each UTXO casting a vote
        // towards each proposal. This may require multiple txns
        // to properly cast all votes across all proposals.
        //
        // A single input from each unique address is required to
        // prove ownership over the associated utxo. Each OP_RETURN
        // vote must contain the signature generated from the
        // associated utxo casting the vote.

        // Store all voting transactions counter
        int txCounter{0};

        // Store the utxos that are associated with votes map<utxo, proposal hash>
        std::map<COutPoint, std::set<uint256>> usedUtxos;

        // Minimum vote input amount
        const auto voteMinAmount = static_cast<CAmount>(gArgs.GetArg("-voteinputamount", VOTING_UTXO_INPUT_AMOUNT));

        for (auto & wallet : wallets) {
            auto locked_chain = wallet->chain().lock();
            LOCK(wallet->cs_wallet);

            bool completelyDone{false}; // no votes left
            do {
                // Obtain all valid coin from this wallet that can be used in casting votes
                std::vector<COutput> coins;
                wallet->AvailableCoins(*locked_chain, coins);
                std::sort(coins.begin(), coins.end(), [](const COutput & out1, const COutput & out2) -> bool { // sort ascending (smallest first)
                    return out1.GetInputCoin().txout.nValue < out2.GetInputCoin().txout.nValue;
                });

                // Do not proceed if no inputs were found
                if (coins.empty())
                    break;

                // Filter the coins that meet the minimum requirement for utxo amount. These
                // inputs are used as the inputs to the vote transaction. Need one unique
                // input per address in the wallet that's being used in voting.
                std::map<CKeyID, const COutput*> inputCoins;

                // Select the coin set that meets the utxo amount requirements for use with
                // vote outputs in the tx.
                std::vector<COutput> filtered;
                for (const auto & coin : coins) {
                    if (!coin.fSpendable)
                        continue;
                    CTxDestination dest;
                    if (!ExtractDestination(coin.GetInputCoin().txout.scriptPubKey, dest))
                        continue;
                    // Input selection assumes "coins" is sorted ascending by nValue
                    const auto & addr = boost::get<CKeyID>(dest);
                    if (!inputCoins.count(addr) && coin.GetInputCoin().txout.nValue >= static_cast<CAmount>((double)voteMinAmount*0.6)) {
                        inputCoins[addr] = &coin; // store smallest coin meeting vote input amount requirement
                        continue; // do not use in the vote b/c it's being used in the input
                    }
                    if (coin.GetInputCoin().txout.nValue < params.voteMinUtxoAmount)
                        continue;
                    filtered.push_back(coin);
                }

                // Do not proceed if no coins or inputs were found
                if (filtered.empty() || inputCoins.empty())
                    break;

                // Store all the votes for each proposal across all participating utxos. Each
                // utxo can be used to vote towards each proposal.
                std::vector<CRecipient> voteOuts;

                bool doneWithPendingVotes{false}; // do we have any votes left

                // Create all votes, i.e. as many that will fit in a single transaction
                for (int i = 0; i < static_cast<int>(filtered.size()); ++i) {
                    const auto &coin = filtered[i];

                    CTxDestination dest;
                    if (!ExtractDestination(coin.GetInputCoin().txout.scriptPubKey, dest))
                        continue;
                    CKey key; // utxo private key
                    {
                        const auto keyid = GetKeyForDestination(*wallet, dest);
                        if (keyid.IsNull())
                            continue;
                        if (!wallet->GetKey(keyid, key))
                            continue;
                    }

                    for (int j = 0; j < static_cast<int>(proposals.size()); ++j) {
                        const auto & pv = proposals[j];
                        const bool utxoAlreadyUsed = usedUtxos.count(coin.GetInputCoin().outpoint) > 0 &&
                                usedUtxos[coin.GetInputCoin().outpoint].count(pv.proposal.getHash()) > 0;
                        if (utxoAlreadyUsed)
                            continue;
                        const bool alreadyVoted = instance().hasVote(pv.proposal.getHash(), coin.GetInputCoin().outpoint);
                        if (alreadyVoted)
                            continue; // skip, already voted

                        // Create and serialize the vote data and insert in OP_RETURN script. The vote
                        // is signed with the utxo that is representing that vote. The signing must
                        // happen before the vote object is serialized.
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        Vote vote(pv.proposal.getHash(), pv.vote, coin.GetInputCoin().outpoint);
                        if (!vote.sign(key)) {
                            LogPrint(BCLog::GOVERNANCE,
                                     "WARNING: Failed to vote on {%s} proposal, utxo signing failed %s",
                                     pv.proposal.getName(), coin.GetInputCoin().outpoint.ToString());
                            continue;
                        }
                        if (!vote.isValid(params)) { // validate vote
                            LogPrint(BCLog::GOVERNANCE, "WARNING: Failed to vote on {%s} proposal, validation failed",
                                     pv.proposal.getName());
                            continue;
                        }
                        ss << vote;
                        voteOuts.push_back({CScript() << OP_RETURN << ToByteVector(ss), 0, false});

                        // Track utxos that already voted on this proposal
                        usedUtxos[coin.GetInputCoin().outpoint].insert(pv.proposal.getHash());

                        // Track whether we're on the last vote, used to break out while(true)
                        completelyDone = (i == filtered.size() - 1 && j == proposals.size() - 1);

                        if (voteOuts.size() == MAX_OP_RETURN_IN_TRANSACTION) {
                            doneWithPendingVotes = !completelyDone;
                            if (doneWithPendingVotes)
                                break;
                        }
                    }

                    // Do not proceed iterating if we can't fit any more votes in the current transaction
                    if (doneWithPendingVotes)
                        break;
                }

                // At this point the code assumes that MAX_OP_RETURN_IN_TRANSACTION is reached
                // or that we've reached the last known vote (last item in all iterations)

                if (voteOuts.empty()) // Handle case where no votes were produced
                    break;

                // Select the inputs for use with the transaction. Also add separate outputs to pay
                // back the vote inputs to their own addresses as change (requires estimating fees).
                CCoinControl cc;
                cc.fAllowOtherInputs = false;
                cc.destChange = CTxDestination(inputCoins.begin()->first); // pay change to the first input coin
                FeeCalculation fee_calc;
                const auto feeBytes = static_cast<unsigned int>(inputCoins.size()*150) + // TODO Blocknet accurate input size estimation required
                                      static_cast<unsigned int>(voteOuts.size()*MAX_OP_RETURN_RELAY);
                CAmount payFee = GetMinimumFee(*wallet, feeBytes, cc, ::mempool, ::feeEstimator, &fee_calc);
                CAmount estimatedFeePerInput = payFee/(CAmount)inputCoins.size();

                // Select inputs and distribute fees equally across the change addresses (paid back to input addresses minus fee)
                for (const auto & inputItem : inputCoins) {
                    cc.Select(inputItem.second->GetInputCoin().outpoint);
                    voteOuts.push_back({GetScriptForDestination({inputItem.first}),
                                        inputItem.second->GetInputCoin().txout.nValue - estimatedFeePerInput,
                                        false});
                }

                // Create and send the transaction
                CReserveKey reservekey(wallet.get());
                CAmount nFeeRequired;
                std::string strError;
                int nChangePosRet = -1;
                CTransactionRef tx;
                if (!wallet->CreateTransaction(*locked_chain, voteOuts, tx, reservekey, nFeeRequired, nChangePosRet, strError, cc)) {
                    *failReasonRet = strprintf("Failed to create the proposal submission transaction: %s", strError);
                    return error(failReasonRet->c_str());
                }

                // Send all voting transaction to the network. If there's a failure
                // at any point in the process, bail out.
                if (wallet->GetBroadcastTransactions() && !g_connman) {
                    *failReasonRet = "Peer-to-peer functionality missing or disabled";
                    return error(failReasonRet->c_str());
                }

                CValidationState state;
                if (!wallet->CommitTransaction(tx, {}, {}, reservekey, g_connman.get(), state)) {
                    *failReasonRet = strprintf("Failed to create the proposal submission transaction, it was rejected: %s", FormatStateMessage(state));
                    return error(failReasonRet->c_str());
                }

                // Store the committed voting transaction
                txsRet.push_back(tx);
                // Clear vote outs
                voteOuts.clear();
                // Increment vote transaction counter
                ++txCounter;

            } while(!completelyDone);
        }

        // If not voting transactions were created, return error
        if (txCounter == 0) {
            *failReasonRet = strprintf("Failed to submit votes, no votes were created, is the wallet unlocked and have sufficient funds? Funds required: %s", FormatMoney(params.voteBalance));
            return error(failReasonRet->c_str());
        }

        return true;
    }

    /**
     * Submits a proposal to the network and returns true. If there's an issue with the proposal or it's
     * not valid false is returned.
     * @param proposal
     * @param params
     * @param tx Transaction containing proposal submission
     * @param failReasonRet Error message (empty if no error)
     * @return
     */
    static bool submitProposal(const Proposal & proposal, const Consensus::Params & params, CTransactionRef & tx, std::string *failReasonRet) {
        if (!proposal.isValid(params)) {
            *failReasonRet = "Proposal is not valid";
            return error(failReasonRet->c_str()); // TODO Blocknet indicate what isn't valid
        }

        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << proposal;

        std::string strAddress = gArgs.GetArg("-proposaladdress", "");
        bool proposalAddressSpecified = !strAddress.empty();

        CTxDestination address;
        if (proposalAddressSpecified) {
            if (!IsValidDestinationString(strAddress)) {
                *failReasonRet = "Bad proposal address specified in 'proposaladdress' config option. Make sure it's a valid legacy address";
                return error(failReasonRet->c_str());
            }
            address = DecodeDestination(strAddress);
            CScript s = GetScriptForDestination(address);
            std::vector<std::vector<unsigned char> > solutions;
            if (Solver(s, solutions) != TX_PUBKEYHASH) {
                *failReasonRet = "Bad proposal address specified in 'proposaladdress' config option. Only p2pkh (pay-to-pubkey-hash) addresses are accepted";
                return error(failReasonRet->c_str());
            }
        }

        bool send{false};
        auto wallets = GetWallets();

        // Iterate over all wallets and attempt to submit proposal fee transaction.
        // If a proposal address is specified via config option and the amount
        // doesn't meet the requirements, the proposal transaction will not be sent.
        // The first valid wallet that succeeds in creating a valid proposal tx
        // will be used. This does not support sending transactions with inputs
        // shared across multiple wallets.
        for (auto & wallet : wallets) {
            auto locked_chain = wallet->chain().lock();
            LOCK(wallet->cs_wallet);

            const auto & balance = wallet->GetAvailableBalance();
            if (balance <= params.proposalFee || wallet->IsLocked())
                continue;

            if (wallet->GetBroadcastTransactions() && !g_connman) {
                *failReasonRet = "Peer-to-peer functionality missing or disabled";
                return error(failReasonRet->c_str());
            }

            // Sort coins ascending to use up all the undesirable utxos
            std::vector<COutput> coins;
            wallet->AvailableCoins(*locked_chain, coins, true);
            if (coins.empty())
                continue;

            CCoinControl cc;
            if (proposalAddressSpecified) { // if a specific proposal address was specified, only spend from that address
                // Sort ascending
                std::sort(coins.begin(), coins.end(), [](const COutput & out1, const COutput & out2) -> bool {
                    return out1.GetInputCoin().txout.nValue < out2.GetInputCoin().txout.nValue;
                });

                CAmount selectedAmount{0};
                for (const COutput & out : coins) { // add coins to cover proposal fee
                    if (!out.fSpendable)
                        continue;
                    CTxDestination dest;
                    if (!ExtractDestination(out.GetInputCoin().txout.scriptPubKey, dest))
                        continue;
                    if (dest != address)
                        continue; // skip if address isn't proposal address
                    cc.Select(out.GetInputCoin().outpoint);
                    selectedAmount += out.GetInputCoin().txout.nValue;
                    if (selectedAmount > params.proposalFee)
                        break;
                }

                if (selectedAmount <= params.proposalFee)
                    continue; // bail out if not enough funds (need to account for network fee, i.e. > proposalFee)

            } else { // set change address to address of largest utxo
                std::sort(coins.begin(), coins.end(), [](const COutput & out1, const COutput & out2) -> bool {
                    return out1.GetInputCoin().txout.nValue > out2.GetInputCoin().txout.nValue; // Sort descending
                });
                for (const auto & coin : coins) {
                    if (ExtractDestination(coin.GetInputCoin().txout.scriptPubKey, address))
                        break;
                }
            }
            cc.destChange = address;

            // Create and send the transaction
            CReserveKey reservekey(wallet.get());
            CAmount nFeeRequired;
            std::string strError;
            std::vector<CRecipient> vecSend;
            int nChangePosRet = -1;
            CRecipient recipient = {CScript() << OP_RETURN << ToByteVector(ss), params.proposalFee, false};
            vecSend.push_back(recipient);
            if (!wallet->CreateTransaction(*locked_chain, vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError, cc)) {
                CAmount totalAmount = params.proposalFee + nFeeRequired;
                if (totalAmount > balance) {
                    *failReasonRet = strprintf("This transaction requires a transaction fee of at least %s: %s", FormatMoney(nFeeRequired), strError);
                    return error(failReasonRet->c_str());
                }
                return error("Failed to create the proposal submission transaction: %s", strError);
            }

            CValidationState state;
            if (!wallet->CommitTransaction(tx, {}, {}, reservekey, g_connman.get(), state)) {
                *failReasonRet = strprintf("Failed to create the proposal submission transaction, it was rejected: %s", FormatStateMessage(state));
                return error(failReasonRet->c_str());
            }

            send = true;
            break; // done
        }

        if (!send) {
            *failReasonRet = strprintf("Failed to create proposal, check that your wallet is unlocked with a balance of at least %s", FormatMoney(params.proposalFee));
            return error(failReasonRet->c_str());
        }

        return true;
    }

    /**
     * Fetch the list of all proposals since the specified block. Requires loadGovernanceData to have been run
     * on chain load.
     * @param sinceBlock
     * @param allProposals
     * @param allVotes
     */
    static void getProposalsSince(const int & sinceBlock, std::vector<Proposal> & allProposals, std::vector<Vote> & allVotes) {
        auto proposals = gov::Governance::instance().getProposals();
        auto votes = gov::Governance::instance().getVotes();
        for (const auto & p : proposals) {
            if (p.getBlockNumber() >= sinceBlock)
                allProposals.push_back(p);
        }
        for (const auto & v : votes) {
            if (v.getBlockNumber() >= sinceBlock)
                allVotes.push_back(v);
        }
    }

protected:
    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex,
                        const std::vector<CTransactionRef>& txn_conflicted) override
    {
        std::set<Proposal> ps;
        std::set<Vote> vs;
        dataFromBlock(block.get(), ps, vs, pindex); // excludes votes/proposals that don't meet cutoffs
        const auto & consensus = Params().GetConsensus();
        const auto nextSB = nextSuperblock(pindex->nHeight, consensus);
        {
            LOCK(mu);
            for (auto & proposal : ps)
                proposals[proposal.getHash()] = proposal;
            for (auto & vote : vs) {
                if (!proposals.count(vote.getProposal()))
                    continue; // skip votes without valid proposals
                // Handle vote changes, if a vote already exists and the user
                // is submitting a change, only count the vote with the most
                // recent timestamp. If a vote on the same utxo occurs in the
                // same block, the vote with the larger hash is chosen as the
                // tie breaker. This could have unintended consequences if the
                // user intends the smaller hash to be the most recent vote.
                // The best way to handle this is to build the voting client
                // to require waiting at least 1 block between vote changes.
                // Changes to this code below must also be applied to "dataFromBlock()"
                if (votes.count(vote.getHash())) {
                    if (vote.getTime() > votes[vote.getHash()].getTime())
                        votes[vote.getHash()] = vote;
                    else if (UintToArith256(vote.sigHash()) > UintToArith256(votes[vote.getHash()].sigHash()))
                        votes[vote.getHash()] = vote;
                } else // if no vote exists then add
                    votes[vote.getHash()] = vote;
            }
            // Remove any spent votes, i.e. any votes that have had their
            // utxos spent in this block. We'll store all the vin prevouts
            // and then check any votes that share those utxos to determine
            // if they're invalid.
            std::set<COutPoint> prevouts;
            for (const auto & tx : block->vtx) {
                for (const auto & vin : tx->vin)
                    prevouts.insert(vin.prevout);
            }
            for (auto it = votes.cbegin(); it != votes.cend(); ) {
                if (prevouts.count(it->second.getUtxo()))
                    votes.erase(it++); // vote utxo was spent in vin, remove it
                else
                    ++it;
            }
        }
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block) override {
        std::set<Proposal> ps;
        std::set<Vote> vs;
        dataFromBlock(block.get(), ps, vs); // cutoff check disabled here b/c we're disconnecting
                                            // already validated votes/proposals
        {
            LOCK(mu);
            for (auto & proposal : ps)
                proposals.erase(proposal.getHash());
            for (auto & vote : vs)
                votes.erase(vote.getHash());
        }
    }

protected:
    Mutex mu;
    std::map<uint256, Proposal> proposals GUARDED_BY(mu);
    std::map<uint256, Vote> votes GUARDED_BY(mu);
};

}

#endif //BLOCKNET_GOVERNANCE_H
