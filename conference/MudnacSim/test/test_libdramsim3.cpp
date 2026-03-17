#include <iostream>
#include <cstdio>
#include <dramsim3.h>


#define MAX_ADDR_LIST_SIZE (1024 * 16 * 1024)


uint64_t addrList[MAX_ADDR_LIST_SIZE];
uint64_t remainedTransCount;
uint64_t cycles;


void readCallback(uint64_t);
void writeCallback(uint64_t);


int main() {
    std::cout << "main starts" << std::endl;

    dramsim3::MemorySystem* msPtr = dramsim3::GetMemorySystem(
            "DRAMsim3/configs/DDR4_8Gb_x16_3200_am1.ini",
            "test/output_test_libdramsim3",
            readCallback,
            writeCallback
    );

    if (msPtr == NULL) {
        std::cout << "Something went wrong during dramsim3::GetMemorySystem" << std::endl;
    } else {
        std::cout << "MemorySystem is created." << std::endl;
        dramsim3::MemorySystem& ms = *(msPtr);

        printf("GetTCK=%lf(ns)\n", ms.GetTCK());
        printf("GetBusBits=%d(bits)\n", ms.GetBusBits());
        printf("GetBurstLength=%d(beats)\n", ms.GetBurstLength());
        printf("GetQueueSize=%d\n", ms.GetQueueSize());

        uint64_t burstBytes = ms.GetBusBits() / 8 * ms.GetBurstLength();

        uint64_t baseAddress = 0x111100;
//        uint64_t size = 16;
//        uint64_t size = 64;
//        uint64_t size = 256;
//        uint64_t size = 512;
//        uint64_t size = 1024;
        uint64_t size = 16384;
//        uint64_t size = 16384 * 16;
//        uint64_t size = MAX_ADDR_LIST_SIZE;
        for (int i = 0; i < size; i++) {
//            addrList[i] = baseAddress + i * burstBytes / 4;
//            addrList[i] = baseAddress + i * burstBytes / 2;
            addrList[i] = baseAddress + i * burstBytes;
//            addrList[i] = baseAddress + i * burstBytes * 2;
//            addrList[i] = baseAddress + i * burstBytes * 4;
//            addrList[i] = baseAddress + i * burstBytes * 8;
//            addrList[i] = baseAddress + i * burstBytes * 16;
//            addrList[i] = baseAddress + i * burstBytes * 32;
        }

//        bool isWrite = true;
        bool isWrite = false;

        remainedTransCount = size;
        cycles = 0;
        for (int i = 0; i < size; i++) {
            while(!ms.WillAcceptTransaction(addrList[i], isWrite)){
                ms.ClockTick();
                cycles++;
            }
            ms.AddTransaction(addrList[i], isWrite);
            ms.ClockTick();
            cycles++;
        }
        while (remainedTransCount > 0) {
            ms.ClockTick();
            cycles++;
        }

        printf("Trans=%ld, Cycles=%ld\n", size, cycles);
        printf("Bandwidth=%lf(Bytes/Cycles)\n", (double)size * burstBytes / cycles);
        printf("Bandwidth=%lf(GB/s) (This should be the same as in dramsim3.txt)\n",
               (double)size * burstBytes / cycles / ms.GetTCK());

        ms.PrintStats();
        delete msPtr;
        std::cout << "MemorySystem is deleted." << std::endl;
    }

    std::cout << "main ends" << std::endl;
    return 0;
}


void readCallback(uint64_t addr) {
//    printf("%lu %lx\n", cycles, addr);
    remainedTransCount--;
}


void writeCallback(uint64_t addr) {
//    printf("%lu %lx\n", cycles, addr);
    remainedTransCount--;
}
