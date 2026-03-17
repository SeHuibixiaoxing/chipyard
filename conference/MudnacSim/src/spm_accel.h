#ifndef MUDNACSIM_SPM_ACCEL_H
#define MUDNACSIM_SPM_ACCEL_H


#include "tools.h"
#include "base_accel.h"
#include "spm.h"


namespace mudnac {


    class SPMAccel : public BaseAccel {
    public:
        class PageTable {
        public:
            uint pageForBypass;
            uint accelId;

            PageTable(uint pageIdxOffset_, uint accelId): accelId(accelId) {
                assert(pageIdxOffset_ >= 0);
                pageIdxOffset = pageIdxOffset_;
                inPageMask = ((uint) 1 << pageIdxOffset_) - 1;

            }

            inline void set(uint vPage, uint pPage, bool v) {
                // assert(vPage < numPages);
                pages[vPage] = pPage;
                valid[vPage] = v;
            }

            inline void invalidPage(uint vPage) {
                // assert(vPage < numPages);
                pages.erase(vPage);
                valid[vPage] = false;
            }

            inline uint getOffset(uint vAddr) {
                return vAddr & inPageMask;
            }

            inline uint getPPage(uint vAddr) {
                uint vPage = vAddr >> pageIdxOffset;
                // assert(vPage < numPages);
                if (!valid[vPage]) {
                    std::cout << "vPage=" << vPage 
                                << ", accelId=" << accelId 
                                << std::endl;
                    assert(valid[vPage]);
                }
                return pages[vPage];
            }

            inline uint concat(uint page, uint offset) {
                assert(offset <= inPageMask);
                return (page << pageIdxOffset) + offset;
            }

        private:
            uint pageIdxOffset;
            uint inPageMask;
            // std::vector<uint> pages;
            std::unordered_map<uint, uint> pages;
            std::unordered_map<uint, bool> valid;
        };

        PageTable pageTable;

        SPMAccel(const AccelConfig &config_);

        void assignMasterPort(Port &masterPort_) {
            this->masterPort = &masterPort_;
        }

        void connectCPUPort(std::deque<AccelTiledCMD *> *input, std::deque<AccelTiledCMD *> *output) {
            assert(input != NULL and output != NULL);
            cpuPortInput = input;
            cpuPortOutput = output;
        }


        void tick();

        void setPageTable(uint vPage, uint pPage, bool valid) {
            pageTable.set(vPage, pPage, valid);
        }

        void invalidPageTable(uint vPage) {
            pageTable.invalidPage(vPage);
        }

    private:
        void cmdFetch();
        void updateCmdCountr();

        bool tickCMD(AccelTiledCMD* cmd, bool& msgCreationDone, uint64& flyingSpmMsgCount, uint cmdSrc, uint64& idealBytes, uint64& realBytes); //return if the cmd have been finished.

        std::deque<AccelTiledCMD *> *cpuPortInput;
        std::deque<AccelTiledCMD *> *cpuPortOutput;

        Port *masterPort;

        AccelTiledCMD *computeCMD;
        AccelTiledCMD *loadCMD;
        AccelTiledCMD *storeCMD;
        AccelTiledCMD *waitedComputeCMD;
        AccelTiledCMD *waitedLoadCMD;
        AccelTiledCMD *waitedStoreCMD;

        AccelTiledCMD *sendCMD;
        AccelTiledCMD *flushCMD;
        AccelTiledCMD *fetchCMD;

        // cmd srouce
        static const uint FROM_LOAD_CMD = 1;
        static const uint FROM_COMPUTE_CMD = 2;
        static const uint FROM_STORE_CMD = 3;
        static const uint FROM_SEND_CMD = 4;
        static const uint FROM_FLUSH_CMD = 5;
        static const uint FROM_FETCH_CMD = 6;


        // for spm bank indexing
        uint64 bankIdxOffset;
        uint64 bankIdxMask;
        uint pagesPerBank;
        uint accelsPerBank;

        // for fetchLoad/loadBypass/load
        bool loadSpmMsgCreationDone;
        uint64 flyingLoadSpmMsgCount;

        // for storeBypass/store
        bool storeSpmMsgCreationDone;
        uint64 flyingStoreSpmMsgCount;

        // for spmSend
        bool spmSendCmdCreationDone;
        uint64 flyingSendSpmMsgCount;

        // for spmFetch
        bool spmFetchCmdCreationDone;
        uint64 flyingFetchSpmMsgCount;

        // for spmFlush
        bool spmFlushCmdCreationDone;
        uint64 flyingFlushSpmMsgCount;

        // for compute
        uint64 computeCurrentCycles;
        uint64 computeTotalCycles;

        // for burst to master Port dequeue.
        QueueWithArbitor<SPMMessage *> queueWithArbitor;
        static const uint MASTERPORT_FIFO_LOAD = 0;
        static const uint MASTERPORT_FIFO_FETCH_LOAD = 1;
        static const uint MASTERPORT_FIFO_LOAD_BYPASS = 2;
        static const uint MASTERPORT_FIFO_STORE = 3;
        static const uint MASTERPORT_FIFO_STORE_BYPASS = 4;
        static const uint MASTERPORT_FIFO_SEND = 5;
        static const uint MASTERPORT_FIFO_FLUSH = 6;
        static const uint MASTERPORT_FIFO_FETCH = 7;

        // page table
        AccelTiledCMD *ptCMD;

        Counter::TimeSlice ts, flushTs, sendTs, fetchTs;

        void sendMessageToMasterOutputFifos(SPMMessage* msg);
        
        static bool print_info;
    };


} // mudnac


#endif //MUDNACSIM_SPM_ACCEL_H
