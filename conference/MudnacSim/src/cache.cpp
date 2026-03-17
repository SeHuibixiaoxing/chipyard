#include "cache.h"


namespace mudnac {


    CacheTagSet::CacheTagSet(uint nWays) {
        nWays_ = nWays;
        tags_.reserve(nWays_);
        victimListIterVec_.reserve(nWays_);
        for (int i = 0; i < nWays_; i++) {
            tags_.emplace_back();
            victimTagIdxList_.emplace_front(i);  // nWays-1 -> ... -> 3 -> 2 -> 1 -> 0
            victimListIterVec_.emplace_back();
        }
        int i = nWays_ - 1;
        for (auto iter = victimTagIdxList_.begin(); iter != victimTagIdxList_.end(); iter++) {
            victimListIterVec_[i] = iter;
            i--;
        }
    }

    bool CacheTagSet::check(uint64 tagVal, bool isWrite, bool writeThrough) {
        if (tagVal2TagIdxMap_.count(tagVal)) {  // hit
            uint idx = tagVal2TagIdxMap_[tagVal];
            if (isWrite and (not writeThrough)) {
                tags_[idx].dirty = true;  // update dirty bit
            }
            // next three lines: move the idx to the front of the list (LRU)
            victimTagIdxList_.erase(victimListIterVec_[idx]);
            victimTagIdxList_.push_front(idx);
            victimListIterVec_[idx] = victimTagIdxList_.begin();
            return true;
        }
        return false;
    }

    bool CacheTagSet::victim(uint &idx, uint64 &dirtyTagVal) {
        if (!victimTagIdxList_.empty()) {  // has available victim idx
            idx = victimTagIdxList_.back();  // get victim idx from the back of the list (LRU)
            victimTagIdxList_.pop_back();  // remove this idx from the list (LRU)
            CacheTag &tag = tags_[idx];
            if (tag.valid) {  // has data stored in this idx
                tagVal2TagIdxMap_.erase(tag.value);  // from this idx from tagVal2TagIdxMap
                if (tag.dirty) {  // is dirty data
                    dirtyTagVal = tag.value;
                } else {
                    dirtyTagVal = UINT64_MAX;  // set dirtyTagVal to illegal
                }
            } else {  // no data stored in this idx
                dirtyTagVal = UINT64_MAX;  // set dirtyTagVal to illegal
            }
            return true;
        } else {  // no victim idx is available (because too many misses happened)
            idx = nWays_;  // set idx to illegal
            dirtyTagVal = UINT64_MAX;  // set dirtyTagVal to illegal
            return false;
        }
    }

    void CacheTagSet::update(uint64 tagVal, uint idx) {
        CacheTag &tag = tags_[idx];
        tag.value = tagVal;
        tag.valid = true;
        tag.dirty = false;
        tagVal2TagIdxMap_[tagVal] = idx;
        // next two lines: add this idx to the front of the list (LRU)
        victimTagIdxList_.push_front(idx);
        victimListIterVec_[idx] = victimTagIdxList_.begin();
    }

    bool CacheTagSet::flush(uint idx, uint64 &tagVal) {
        CacheTag &tag = tags_[idx];
        bool flush = tag.valid and tag.dirty;
        tagVal = tag.value;
        tag.value = 0;
        tag.valid = false;
        tag.dirty = false;
        // next three lines: move the idx to the front of the list (LRU)
        victimTagIdxList_.erase(victimListIterVec_[idx]);
        victimTagIdxList_.push_front(idx);
        victimListIterVec_[idx] = victimTagIdxList_.begin();
        return flush;
    }


    CacheBank::CacheBank(uint nSets, uint nWays, uint nBanks, uint lineBytes, uint bankIdx,
                         uint upstreamBeatBytes, uint downstreamBeatBytes, uint memCtrlChannels,
                         bool writeThrough, bool writeAllocation) {
        nSets_ = nSets;
        nWays_ = nWays;
        nBanks_ = nBanks;
        lineBytes_ = lineBytes;
        writeThrough_ = writeThrough;
        writeAllocation_ = writeAllocation;

        bankIdx_ = bankIdx;
        bankIdxOffset_ = (uint) log2(lineBytes);  // the lowest bits next to byte index are for bank index
        setIdxOffset_ = bankIdxOffset_ + (uint) log2(nBanks_);
        tagOffset_ = setIdxOffset_ + (uint) log2(nSets_);
        bankIdxMask_ = nBanks_ - 1;
        setIdxMask_ = nSets_ - 1;
        alignedAddrMask_ = UINT64_MAX - (lineBytes_ - 1);

        memCtrlChIdxMask = memCtrlChannels - 1;
        memCtrlChIdxOffset = log2(lineBytes);

        sets_.reserve(nSets_);
        for (int i = 0; i < nSets_; i++) {
            sets_.emplace_back(nWays_);
        }

        masterPort = NULL;
        slavePort = NULL;

        flyingMemCtrlMsgCount = 0;

        flushMsg = NULL;
        flushFinishing = false;
        flushSetIdx_ = 0;
        flushWayIdx_ = 0;

#ifdef CACHE_DBG
        printf("CacheBank %u: bankIdxOffset=%d, setIdxOffset=%d, tagOffset=%d, bankIdxMask=%lu, setIdxMask=%lu, "
               "alignedAddrMask=%lx\n",
               bankIdx_, bankIdxOffset_, setIdxOffset_, tagOffset_, bankIdxMask_, setIdxMask_,
               alignedAddrMask_);
#endif
    }

    void CacheBank::tick() {
        // part1: receive CacheMessage from upstream
        std::deque<Message *> *fifo = NULL;
        if (not missedCacheMsgFifo_.empty()) {
            fifo = &missedCacheMsgFifo_;
        } else if (not slavePort->input->empty()) {
            fifo = slavePort->input;
        }
        if (flushMsg == NULL and fifo != NULL) {
            CacheMessage *cacheMsg = dynamic_cast<CacheMessage*>(fifo->front());
            if (cacheMsg->flush) {  // flush
                assert(missedCacheMsgFifo_.empty());
                if (mshr_.empty()) {
                    assert(flyingMemCtrlMsgCount == 0);
                    flushMsg = cacheMsg;
                    fifo->pop_front();
#ifdef CACHE_DBG
                    printf("%lu CacheBank %u: receive a flush msg\n", counter.cycles, bankIdx_);
#endif
                }
                goto GOTO_IS_FLUSH_MSG;
            }
            uint64 alignedAddr = cacheMsg->addr & alignedAddrMask_;
            uint64 setIdx = (cacheMsg->addr >> setIdxOffset_) & setIdxMask_;
            uint64 tagVal = cacheMsg->addr >> tagOffset_;
            bool hit = sets_[setIdx].check(tagVal, cacheMsg->isWrite, writeThrough_);
#ifdef CACHE_DBG
            printf("%lu CacheBank %u: hit check, addr=%lx, setIdx=%lu, tagVal=%lx, hit=%d\n",
                   counter.cycles, bankIdx_, cacheMsg->addr, setIdx, tagVal, hit);
#endif
            if (hit) {  // hit
                counter.recordUpstream(cacheMsg->isWrite, cacheMsg->size);
                slavePort->output->push_back(cacheMsg);
                fifo->pop_front();
                if (fifo == slavePort->input) {
                    counter.recordHit(cacheMsg->isWrite);
                }
                if (writeThrough_ and cacheMsg->isWrite) {  // write through
                    MemCtrlMessage *memMsg = new MemCtrlMessage(alignedAddr, true);
                    memMsg->setTrans(1, lineBytes_, PortIDManager::localToGlobalCacheMaster(bankIdx_),
                                    PortIDManager::localToGlobalMEMSlave((memMsg->addr >> memCtrlChIdxOffset) & memCtrlChIdxMask));
                    flyingMemCtrlMsgCount++;
                    counter.recordDownstream(memMsg->isWrite, lineBytes_);
                    masterPort->output->push_back(memMsg);
                }
            } else {  // miss
                if (writeThrough_ and (not writeAllocation_) and cacheMsg->isWrite) {
                    // write through, no write allocation
                    counter.recordUpstream(cacheMsg->isWrite, cacheMsg->size);
                    slavePort->output->push_back(cacheMsg);
                    fifo->pop_front();
                    if (fifo == slavePort->input) {
                        counter.recordMiss(cacheMsg->isWrite);
                    }
                    MemCtrlMessage *memMsg = new MemCtrlMessage(alignedAddr, true);
                    memMsg->setTrans(1, lineBytes_, PortIDManager::localToGlobalCacheMaster(bankIdx_),
                                    PortIDManager::localToGlobalMEMSlave((memMsg->addr >> memCtrlChIdxOffset) & memCtrlChIdxMask));
                    flyingMemCtrlMsgCount++;
                    counter.recordDownstream(memMsg->isWrite, lineBytes_);
                    masterPort->output->push_back(memMsg);
                    goto GOTO_NO_WRITE_ALLOCATION;
                }
                if (mshr_.count(alignedAddr)) {  // secondary miss
                    mshr_[alignedAddr].miss_.push_back(cacheMsg);
                    fifo->pop_front();
                    if (fifo == slavePort->input) {
                        counter.recordMiss(cacheMsg->isWrite);
                    }
#ifdef CACHE_DBG
                    printf("%lu CacheBank %u: secondary miss, alignedAddr=%lx\n",
                           counter.cycles, bankIdx_, alignedAddr);
#endif
                } else {  // first miss
                    uint victimTagIdx = nWays_;
                    uint64 dirtyTagVal = UINT64_MAX;
                    bool hasVictim = sets_[setIdx].victim(victimTagIdx, dirtyTagVal);
                    if (hasVictim) {  // has available victim
                        if (dirtyTagVal < UINT64_MAX) {  // has dirty data to write back
                            uint64 dirtyAddr = (dirtyTagVal << tagOffset_) + (setIdx << setIdxOffset_) +
                                               (bankIdx_ << bankIdxOffset_);
                            MemCtrlMessage *memMsg = new MemCtrlMessage(dirtyAddr, true);
                            memMsg->setTrans(1, lineBytes_, PortIDManager::localToGlobalCacheMaster(bankIdx_),
                                             PortIDManager::localToGlobalMEMSlave((memMsg->addr >> memCtrlChIdxOffset) & memCtrlChIdxMask));
                            flyingMemCtrlMsgCount++;
                            counter.recordDownstream(memMsg->isWrite, lineBytes_);
                            masterPort->output->push_back(memMsg);
                        }
                        MemCtrlMessage *memMsg = new MemCtrlMessage(alignedAddr, false);
                        memMsg->setTrans(lineBytes_, 1, PortIDManager::localToGlobalCacheMaster(bankIdx_),
                                            PortIDManager::localToGlobalMEMSlave((memMsg->addr >> memCtrlChIdxOffset) & memCtrlChIdxMask));
                        flyingMemCtrlMsgCount++;
                        counter.recordDownstream(memMsg->isWrite, lineBytes_);
                        masterPort->output->push_back(memMsg);
                        MSHR &mshr = mshr_[alignedAddr];
                        mshr.victimTagIdx_ = victimTagIdx;
                        mshr.miss_.push_back(cacheMsg);
                        fifo->pop_front();
                        if (fifo == slavePort->input) {
                            counter.recordMiss(cacheMsg->isWrite);
                        }
                    }
#ifdef CACHE_DBG
                    printf("%lu CacheBank %u: first miss, "
                           "alignedAddr=%lx, hasVictim=%d, victimTagIdx=%d, dirty=%d, "
                           "dirtyAddr=%lx\n",
                           counter.cycles, bankIdx_,
                           alignedAddr, hasVictim, victimTagIdx, dirtyTagVal < UINT64_MAX,
                           (dirtyTagVal << tagOffset_) + (setIdx << setIdxOffset_) + (bankIdx_ << bankIdxOffset_));
#endif
                }
                GOTO_NO_WRITE_ALLOCATION:;
            }
        }
        GOTO_IS_FLUSH_MSG:;

        // part2: check if any MemCtrlMessage is done by downstream
        if (not masterPort->input->empty()) {
            MemCtrlMessage *msg = dynamic_cast<MemCtrlMessage*>(masterPort->input->front());
            masterPort->input->pop_front();
            if (not msg->isWrite) {  // missed block is returned from downstream
                assert(mshr_.count(msg->addr));
                MSHR &mshr = mshr_[msg->addr];
                uint64 setIdx = (msg->addr >> setIdxOffset_) & setIdxMask_;
                uint64 tagVal = msg->addr >> tagOffset_;
                sets_[setIdx].update(tagVal, mshr.victimTagIdx_);
                for (auto miss: mshr.miss_) {
                    missedCacheMsgFifo_.push_back(miss);
                }
//                printf("%lu CacheBank %u: miss block returned, "
//                       "addr=%lx, victimTagIdx=%d, setIdx=%ld, tagVal=%lx, misses=%zu\n",
//                       counter.cycles, bankIdx_,
//                       msg->addr, mshr.victimTagIdx_, setIdx, tagVal, mshr.miss_.size());
                mshr_.erase(msg->addr);
            } else {
//                printf("%lu CacheBank %u: write back completed, addr=%lx\n",
//                       counter.cycles, bankIdx_, msg->addr);
            }
            delete msg;
            assert(flyingMemCtrlMsgCount > 0);
            flyingMemCtrlMsgCount--;
        }

        // part5: flushing
        if (flushMsg != NULL) {
            if (not flushFinishing) {
                uint64 dirtyTagVal = UINT64_MAX;
                bool needFlush = sets_[flushSetIdx_].flush(flushWayIdx_, dirtyTagVal);
                if (needFlush) {
                    uint64 addr = (dirtyTagVal << tagOffset_) + (flushSetIdx_ << setIdxOffset_) +
                                  (bankIdx_ << bankIdxOffset_);
                    MemCtrlMessage *memMsg = new MemCtrlMessage(addr, true);
                    memMsg->setTrans(1, lineBytes_, PortIDManager::localToGlobalCacheMaster(bankIdx_),
                                    PortIDManager::localToGlobalMEMSlave((memMsg->addr >> memCtrlChIdxOffset) & memCtrlChIdxMask));
                    flyingMemCtrlMsgCount++;
                    counter.recordDownstream(memMsg->isWrite, lineBytes_);
                    masterPort->output->push_back(memMsg);
#ifdef CACHE_DBG
                    printf("%lu CacheBank %u: flushing, setIdx=%u, wayIdx=%u, addr=%lx\n",
                           counter.cycles, bankIdx_, flushSetIdx_, flushWayIdx_, addr);
#endif
                }
                if (flushWayIdx_ == nWays_ - 1) {
                    flushWayIdx_ = 0;
                    if (flushSetIdx_ == nSets_ - 1) {
                        flushSetIdx_ = 0;
                        flushFinishing = true;
                    } else {
                        flushSetIdx_++;
                    }
                } else {
                    flushWayIdx_++;
                }
            } else {
                if (flyingMemCtrlMsgCount == 0) {
                    slavePort->output->push_back(flushMsg);
                    flushMsg = NULL;
                    flushFinishing = false;
                }
            }
        }

        counter.tick();
    }


}  // mudnac
