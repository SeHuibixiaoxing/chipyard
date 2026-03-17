#ifndef MUDNACSIM_TOOLS_H
#define MUDNACSIM_TOOLS_H


//#define CACHE_DBG
//#define TENSOR_DBG
//#define BASE_ACCEL_DBG
//#define CACHE_ACCEL_DBG
// #define SPM_ACCEL_DBG
//#define TILE_LEVEL_PROGRESS
// #define RUNTIME_DBG
// #define RUNTIME_MODEL_DBG
#define THREAD_DBG
//#define LAYER_DBG
// #define PERF_DETAIL_LOG

//#define NDEBUG  // disable assert(...)

// #define JSON_DBG



#include <iostream>
#include <string>
#include <sstream>
#include <cstdio>
#include <cassert>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <cmath>
#include <array>
#include <vector>
#include <list>
#include <map>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <tuple>
#include <deque>
#include <pthread.h>
#include <random>
#include <algorithm>
#include <unistd.h>
#include <utility>
#include <memory>
#include <type_traits>
#include <numeric>
#include <queue>
#include <cfloat>
#include <iomanip>
#include <functional>
#include <nlohmann/json.hpp>


#include "debug.h"

const std::string SPM_DEBUG_FLAG = "SPM";
const std::string MEMCTRL_DEBUG_FLAG = "MEMCTRL";

const std::string WZYDEBUG_FLAG = "WZYDEBUG";

const std::string STAGE_DEBUG_FLAG = "STAGE";
const std::string PIPELINE_DEBUG_FLAG = "PIPELINE";
const std::string PIPELINE_CRITICA_FLAGL = "PIPELINE_CRITICAL";
const std::string STAGE_DEBUG_DETAIL_FLAG = "STAGE_DETAIL";
const std::string PIPELINE_DEBUG_DETAIL_FLAG = "PIPELINE_DETAIL";


const std::string THREAD_PIPELINE = "THREAD_PIPELINE";
const std::string THREAD_QOS_PIPELINE = "THREAD_QOS_PIPELINE";
const std::string RUNTIME_SOURCE_ALLOC = "RUNTIME_SOURCE_ALLOC";

const std::string PIPELINE_DEADLOCK_FLAG = "PIPELINE_DEADLOCK_FLAG";
const uint JSON_DUMP_INDENTATION = 2;

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint uint32;
typedef uint64_t uint64;

typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;


namespace mudnac {

    typedef uint64 TensorId;
    typedef std::vector<uint> PageSet;
    typedef std::shared_ptr<PageSet> PageSetShared;
    typedef std::unique_ptr<PageSet> PageSetUnique;

    inline std::vector<std::string> splitStrBy(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        
        return tokens;
    }   
    inline bool isPowOfTwo(uint64 n) {
        return (n & (n - 1)) == 0;
    }

    inline uint32 uintDiv(uint32 a, uint32 b) {
        assert(b > 0);
        uint32 c = a % b;
        return (c > 0) ? ((a - c) / b + 1) : a / b;
    }

    inline uint32 divCeil(uint32 a, uint32 b) {
        return uintDiv(a, b);
    }

    inline uint32 divFloor(uint32 a, uint32 b) {
        assert(b > 0);
        uint32 c = a % b;
        return (a - c) / b;
    }

    inline uint64 alignUp(uint64 a, uint64 align) {
        uint64 r = a % align;
        return (r > 0) ? (a - r + align) : a;
    }

    inline uint64 alignDown(uint64 a, uint64 align) {
        return a - (a % align);
    }

    inline uint64 addrCeil(uint64 addr, uint64 align) {
        return (addr + (align - 1)) & (UINT64_MAX - (align - 1));
    }


    inline uint64 addrFloor(uint64 addr, uint64 align) {
        return addr & (UINT64_MAX - (align - 1));
    }

    inline uint64 roundupToPowOfTwo(uint64 n) {
        if (n == 0) return 1; // Edge case: if input is 0, return 1.
        n--; // Decrement to handle exact powers of 2.
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32; // Additional shift for 64-bit integers.
        return n + 1;
    }

    template<typename T>
    std::string toString(const std::vector<T> &v) {
        std::stringstream ss;
        ss << "[";
        for (auto i: v) {
            ss << std::to_string(i) << ",";
        }
        ss << "]";
        return ss.str();
    }

    template<typename T, unsigned long N>
    std::string toString(const std::array<T, N> &v) {
        std::vector<T> v_(N);
        for (int i = 0; i < N; i++) {
            v_[i] = v[i];
        }
        return toString(v_);
    }

    template<typename T>
    std::string toString(const std::deque<T> &v) {
        std::vector<T> v_(v.size());
        for (int i = 0; i < v.size(); i++) {
            v_[i] = v[i];
        }
        return toString(v_);
    }

    template<typename T>
    std::string toString(const std::vector<std::vector<T>> &vv) {
        std::stringstream ss;
        ss << "[\n";
        for (auto &v: vv) {
            ss << "\t" << toString(v) << "\n";
        }
        ss << "]";
        return ss.str();
    }

    template<typename T>
    std::string toString(const std::deque<std::deque<T>> &vv) {
        std::vector<std::vector<T>> vv_;
        vv_.resize(vv.size());
        for (uint i = 0; i < vv.size(); i++) {
            vv_[i].resize(vv[i].size());
            for (uint j = 0; j < vv[i].size(); j++) {
                vv_[i][j] = vv[i][j];
            }
        }
        return toString(vv_);
    }

    template<typename T>    
    constexpr int log2Uint(T n) {
        static_assert(
            std::is_same_v<T, uint32> || std::is_same_v<T, uint64>, 
            "T must be uint32 or uint64"
        );
        int bits = 0;
        while (n >>= 1) {  // 右移直到 n 为 0
            ++bits;
        }
        return bits;
    }

    template<typename T>
    constexpr T acculateVectorSize(const std::vector<std::vector<T>>& v, const T& init) {
        return std::accumulate(v.begin(), v.end(), init, [](const auto& sum, const auto& v) {
            return sum + v.size();
        });
    }

    class PortIDManager {
    public:

        static uint localToGlobal(uint idx, uint type, bool isMaster) {
            return (type << log2Uint(MS_NUM*LOCAL_PORT_NUM)) | (idx << log2Uint(MS_NUM)) | (isMaster ? 1 : 0);
        }

        static uint globalToLocal(uint idx, uint type) {
            return (idx & LOCAL_PORT_MASK) >> log2Uint(MS_NUM);
        }

        static uint localToGlobalAccelMaster(uint idx) {return localToGlobal(idx, PORT_TYPE_ACCEL, true);}
        static uint localToGlobalAccelSlave(uint idx) {return localToGlobal(idx, PORT_TYPE_ACCEL, false);}
        static uint localToGlobalSPMMaster(uint idx) {return localToGlobal(idx, PORT_TYPE_SPM, true);}
        static uint localToGlobalSPMSlave(uint idx) {return localToGlobal(idx, PORT_TYPE_SPM, false);}
        static uint localToGlobalMEMMaster(uint idx) {return localToGlobal(idx, PORT_TYPE_MEM_CTRL_CHANNEL, true);}
        static uint localToGlobalMEMSlave(uint idx) {return localToGlobal(idx, PORT_TYPE_MEM_CTRL_CHANNEL, false);}
        static uint localToGlobalCacheMaster(uint idx) {return localToGlobal(idx, PORT_TYPE_CACHE, true);}
        static uint localToGlobalCacheSlave(uint idx) {return localToGlobal(idx, PORT_TYPE_CACHE, false);}


        static uint globalToLocalAccel(uint idx) {return globalToLocal(idx, PORT_TYPE_ACCEL);}
        static uint globalToLocalSPM(uint idx) {return globalToLocal(idx, PORT_TYPE_SPM);}
        static uint globalToLocalMEM(uint idx) {return globalToLocal(idx, PORT_TYPE_SPM);}
        static uint globalToLocalCache(uint idx) {return globalToLocal(idx, PORT_TYPE_CACHE);}

        static uint masterToSlave(uint idx) {assert(isMaster(idx)); return idx ^ MS_MASK;}
        static uint slaveToMaster(uint idx) {assert(isSlave(idx)); return idx ^ MS_MASK;}


        static bool isAccel(uint portId) {
            return isType(portId, PORT_TYPE_ACCEL);
        }
        static bool isSPM(uint portId){
            return isType(portId, PORT_TYPE_SPM);            
        }
        static bool isMemCtrlChannel(uint portId){
            return isType(portId, PORT_TYPE_MEM_CTRL_CHANNEL);
        }       
        static bool isCache(uint portId) {
            return isType(portId, PORT_TYPE_CACHE);
        }

        static bool isMaster(uint portId) {
            return portId & MS_MASK;
        }

        static bool isSlave(uint portId) {
            return !isMaster(portId);
        }

        static uint getPortIDNum() {
            return LOCAL_PORT_NUM * MS_NUM * PORT_TYPE_NUM - 1;
        }

        static uint getMaxPortID() {
            return getPortIDNum() - 1;
        }

    private:
        static const uint MS_NUM = 2;
        static const uint MS_MASK = MS_NUM - 1;
        static const uint LOCAL_PORT_NUM = 128;
        static const uint LOCAL_PORT_MASK = (LOCAL_PORT_NUM-1) << log2Uint(MS_NUM);
        static const uint PORT_TYPE_NUM = 8;
        static const uint PORT_TYPE_MASK = (PORT_TYPE_NUM-1) << (log2Uint(LOCAL_PORT_NUM*MS_NUM));     

        static const uint PORT_TYPE_ACCEL = 0;
        static const uint PORT_TYPE_SPM = 1;
        static const uint PORT_TYPE_MEM_CTRL_CHANNEL = 2;   
        static const uint PORT_TYPE_CACHE = 3;

        static uint getType(uint portId) {
            return (portId & PORT_TYPE_MASK) >> log2Uint(MS_NUM*LOCAL_PORT_NUM);
        }

        static bool isType(uint portId, uint type) {
            return getType(portId) == type;
        }
    };

    /**
     * @brief 消息接口，所有模块间通信均通过该消息传递。每个消息包括up接口和down接口。
     * upport发送upBytes大小请求到downport，downport发送downBytes大小相应到upport。
     * MulticastMsgs用于存储多播消息。
     * 
     */
    class Message {
    public:
        uint metaType;
        static constexpr uint META_TYPE_UNKNOEN = 0;
        static constexpr uint META_TYPE_SPM = 1;
        static constexpr uint META_TYPE_CACHE = 2;
        static constexpr uint META_TYPE_MEMCTRL = 3;
        static constexpr uint META_TYPE_TEST_NOC = 4;
        static constexpr uint META_TYPE_TEST_ROUTER = 5;

        uint64 s2mBytes; // upBytes
        uint64 m2sBytes; // downBytes
        uint masterPortId; // upPortId
        uint slavePortId; // downPortId
        uint64 msg_global_id;

        std::deque<Message *> multicastMsgs; // multiple response messages for a group of requests
        Message(uint64 s2mBytes_, uint64 m2sBytes_, uint masterPortId_, uint slavePortId_, uint metaType_ = META_TYPE_UNKNOEN) {
            s2mBytes = s2mBytes_, m2sBytes = m2sBytes_, masterPortId = masterPortId_, slavePortId = slavePortId_;
            metaType = metaType_;

            msg_global_id = Message::message_global_id_count;
            ++ Message::message_global_id_count;
        }

        Message(uint metaType_ = META_TYPE_UNKNOEN) : Message(0, 0, 0, 0, metaType_) {}

        virtual ~Message() = default;

        inline void setTrans(uint64 s2mBytes_, uint64 m2sBytes_, uint masterPortId_, uint slavePortId_) {
            assert(PortIDManager::isMaster(masterPortId_));
            assert(PortIDManager::isSlave(slavePortId_));
            s2mBytes = s2mBytes_, m2sBytes = m2sBytes_, masterPortId = masterPortId_, slavePortId = slavePortId_;
        }

        inline bool isMulticastRespMsg() { return not multicastMsgs.empty(); }

        virtual void print(const std::string& debugFlag) {
            LOGF(debugFlag, "Message Info: id=%llu\n", msg_global_id);
            LOGF(debugFlag, "meta type: %u, addr(this): %p\n", metaType, static_cast<void *>(this));
            LOGF(debugFlag, "s2mBytes: %#lx, m2sBytes: %#lx, masterPortId: %#x, slavePortId: %#x\n", s2mBytes, m2sBytes, masterPortId, slavePortId);
        }

        inline static uint64 message_global_id_count = 0;
    };

    class Port {
    public:
        std::deque<Message*> *input;
        std::deque<Message*> *output;

        Port(): input(NULL), output(NULL) {}

        bool notNull() {
            return input != NULL && output != NULL;
        }
        
        void connectWith(Port &port) {
            assert(port.notNull());
            this->input = port.input;
            this->output = port.output;
        }
    
        void assign(std::deque<Message*> &input, std::deque<Message*> &output) {
            assignInput(input);
            assignOutput(output);
        }

        void assignInput(std::deque<Message*> &input) {
            this->input = &input;
        }

        void assignOutput(std::deque<Message*> &output) {
            this->output = &output;
        }
    };

    /**
     * @brief This class definition defines an abstract base class TickModule that provides a basic structure for modules that need to perform some action at each tick.
     * 
     */
    class TickModule {
    public:
        virtual ~TickModule() = default;

        virtual void tick() {};
    };

    
    template<typename T>
    class SizedFifo {
    private:
        std::deque<T> q;
        uint64 sz;

    public:
        SizedFifo(uint64 size = 1) : sz(size) {
            assert(size > 0);
        }

        inline void resize(uint64 size) {
            assert(size > 0);
            assert(size >= q.size());
            sz = size;
        }

        inline bool canPush(uint64 n = 1) { return q.size() + n <= sz; }

        inline void push(T t) {
            assert(canPush());
            q.push_back(t);
        }

        inline bool canPop(uint64 n = 1) { return q.size() >= n; }

        inline T pop() {
            assert(canPop());
            T t = q.front();
            q.pop_front();
            return t;
        }

        inline T peek() {
            assert(canPop());
            return q.front();
        }

        inline uint64 size() { return sz; }

        inline uint64 has() { return q.size(); }
    };

    class Arbitor: public TickModule {
    public:
        Arbitor(uint32 count_): count(count_), lastGrant(-1) { enables.resize(count, false); }
        
        void enable(uint id) {
            assert(id < count);
            enables[id] = true;
        }

        void disable(uint id) {
            assert(id < count);
            enables[id] = false;
        }

        int getLastGrant() { return lastGrant; }

        bool getEnable(uint id) {
            assert(id < count);
            return enables[id];
        }

        void tick() {
            generateNextGrant();
        }        

    protected:
        std::vector<bool> enables;
        uint32 count;
        int lastGrant;

        virtual void generateNextGrant() = 0;
    };

    class RRArbitor: public Arbitor {
    public:
        RRArbitor(uint32 count_): Arbitor(count_) {}
    protected:
        void generateNextGrant() final {
            if(lastGrant == -1) {
                for(uint i = 0;i < count;i ++) {
                    if(enables[i]) {
                        lastGrant = i;
                        return;
                    }
                }
                return;
            }
            for(uint i = 1;i <= count;i ++) {
                uint j = (i + lastGrant) % count;
                if (enables[j]) {
                    lastGrant = j;
                    return;
                }                
            }
            lastGrant = -1;
            return;
        }
    };

    template<typename T>
    class QueueWithArbitor: public TickModule {
    public:
        enum class ArbitorType {
            RRArbitor
        };

        QueueWithArbitor(ArbitorType type, uint32 count_, std::string name_ = ""): count(count_), name(name_)
        {  
            multiQueue.resize(count_);
            if(type == ArbitorType::RRArbitor) {
                arbitor = std::make_unique<RRArbitor>(count_);
            }
            hasConsume = false;
        }

        std::tuple<T, int> pop() {
            if(!this->canPop()) {
                return std::make_tuple(T(NULL), -1);
            }
            auto lastGrant = arbitor->getLastGrant();
            auto re = pop_front(lastGrant);
            if(multiQueue[lastGrant].empty()) {
                arbitor->disable(lastGrant);
            }
            hasConsume = true;
            return std::make_tuple(re, lastGrant);
        }

        bool canPop() {
            auto lastGrant = arbitor->getLastGrant();
            return !hasConsume && lastGrant != -1;
        }

        void push(int idx, T elem) {
            push_back(idx, elem);
            arbitor->enable(idx);
        }

        void enable(int idx) {
            assert(idx < count);
            if(multiQueue[idx].empty()) return;
            arbitor->enable(idx);
        }
        void disable(int idx) {
            assert(idx < count);
            arbitor->disable(idx);
        }

        void tick() override {
            arbitor->tick();
            hasConsume = false;
        }

    private:
        std::vector<std::deque<T>> multiQueue;    
        std::unique_ptr<Arbitor> arbitor;    
        int count;
        bool hasConsume;
        std::string name;

        T pop_front(int idx) {
            assert(idx < count);
            assert(!multiQueue[idx].empty());
            auto re = multiQueue[idx].front();
            multiQueue[idx].pop_front();
            return re;
        }
        
        void push_back(int idx, T elem) {
            assert(idx < count);
            multiQueue[idx].push_back(elem);    
        }
    };


    /**
     * @brief This class tracks a series of values over time, divided into periods of a fixed length (periodCycles). 
     * 
     */
    template<typename T>
    class PeriodicalCounter {
    public:
        uint64 periodCycles;
        std::deque<T> dataList;

    private:
        uint64 cycles;

    public:
        PeriodicalCounter(uint64 periodCycles_ = 100) {
            periodCycles = periodCycles_;
            dataList.push_back(0);
            cycles = 0;
        }


        
        /**
         * @brief Adds a data value to the current period's total.
         * 
         * This function increments the most recent entry in the dataList
         * by the specified data value. It is used to accumulate data
         * for the current period until the period is completed.
         * 
         * @param data_ The data value to be added to the current period's total.
         */
        inline void add(const T &data_) {
             dataList.back() += data_; 
        }

        /**
         * @brief Advances the counter by one cycle. 
         * 
         * This function increments the current cycle count. If the cycle count reaches 
         * the period length (periodCycles), it resets the cycle count to zero and appends 
         * a new zero value to the dataList, indicating the start of a new period.
         */
        inline void tick() {
            if (cycles == periodCycles - 1) {
                dataList.push_back(0);
                cycles = 0;
            } else {
                cycles++;
            }
        }

        inline T get(uint idx) { return dataList[idx]; }

        inline uint64 size() { return dataList.size(); }

        inline T sum() {
            T s = 0;
            for (auto &d: dataList) { s += d; }
            return s;
        }

        inline T max() {
            T s = 0;
            for (auto &d: dataList) { s = std::max(s, d); }
            return s;
        }

        inline void merge(PeriodicalCounter<T> &pc) {
            assert(periodCycles == pc.periodCycles);
            cycles = pc.cycles;
            if (pc.size() > size()) { dataList.resize(pc.size(), 0); }
            for (uint64 i = 0; i < std::min(pc.size(), size()); i++) { dataList[i] += pc.dataList[i]; }
        }

        inline void reset() {
            dataList.clear();
            dataList.push_back(0);
            cycles = 0;
        }
    };

    /**
     * @brief This class definition is for a histogram counter, which is a data structure used to count the occurrences of different keys.
     * 
     * @tparam K the type of key
     * @tparam V the type of value
     */
    template<typename K, typename V>
    class HistogramCounter {
    public:
        std::deque<V> counters; 

        ~HistogramCounter() = default;

        virtual uint64 keyToIdx(const K &key) { return (uint64) key; };

        inline uint64 size() { return counters.size(); }

        inline void add(const K &key, const V &value) {
            uint64 idx = keyToIdx(key);
            if (idx >= counters.size()) counters.resize(idx + 1, (V) 0);
            counters[idx] += value;
        }

        inline uint64 getTotalCount() {
            V total = 0;
            for (auto &i: counters) total += i;
            return total;
        }

        inline void merge(HistogramCounter<K, V> &hc) {
            if (hc.size() > size()) { counters.resize(hc.size(), 0); }
            for (uint64 i = 0; i < std::min(hc.size(), size()); i++) { counters[i] += hc.counters[i]; }
        }

        inline std::string toString(uint64 maxIdx = INT32_MAX) {
            std::stringstream ss;
            ss << "[";
            for (uint64 i = 0; i < std::min(maxIdx + 1, size()); i++) {
                ss << i << ":" << counters[i] << ", ";
            }
            if (maxIdx + 1 < size()) {
                V s = 0;
                for (uint64 i = maxIdx + 1; i < size(); i++) {
                    s += counters[i];
                }
                ss << ">" << maxIdx << ":" << s;
            }
            ss << "]";
            return ss.str();
        }

        inline void reset() {
            counters.clear();
        }
    };


    class IntervalManager {
    public:
        IntervalManager(long long min_val, long long max_val)
            : global_min(min_val), global_max(max_val) {
            if (min_val > max_val) {
                throw std::invalid_argument("Invalid interval range");
            }
        }

        long long allocate(long long start, long long size) {
            if (size <= 0) return -1;
            
            long long end = start + size - 1;
            
            if (start < global_min || end > global_max) {
                return -1;
            }
            
            if (isOverlapping(start, end)) {
                return -1;
            }
            
            occupied_intervals.insert({start, end});
            return end;
        }

        bool isOccupied(long long start, long long size) const {
            if (size <= 0) return false;
            
            long long end = start + size - 1;
            
            if (start < global_min || end > global_max) {
                return false;
            }
            
            return isOverlapping(start, end);
        }

        bool release(long long start, long long end) {
            if (end < start) return false;

            auto it = occupied_intervals.find({start, end});
            if (it != occupied_intervals.end()) {
                occupied_intervals.erase(it);
                return true;
            }
            
            return false;
        }

        template<typename T>
        std::pair<long long, long long> getAvailableInterval(std::vector<T> shape) {
            T sum = 1;
            for (auto i : shape) sum *= i;
            return getAvailableInterval(sum);
        }

        std::pair<long long, long long> getAvailableInterval(long long size) {
            if (size <= 0) {
                std::cout << "size <=0 in interval.\n";
                return {-1, -1}; // 无效大小
            }

            long long required_length = size;
            long long prev_end = global_min - 1; // 前一个区间的结束位置（初始为全局最小值前一位）

            // 遍历所有已占用的区间，检查间隙
            for (const auto& interval : occupied_intervals) {
                long long current_start = interval.first;
                long long current_end = interval.second;

                // 计算当前间隙的起始和结束位置
                long long gap_start = prev_end + 1;
                long long gap_end = current_start - 1;

                // 若间隙有效且长度足够
                if (gap_start <= gap_end) {
                    long long gap_length = gap_end - gap_start + 1;
                    if (gap_length >= required_length) {
                        long long alloc_start = gap_start;
                        long long alloc_end = alloc_start + size - 1;
                        occupied_intervals.insert({alloc_start, alloc_end}); // 占用该区间
                        return {alloc_start, alloc_end};
                    }
                }

                prev_end = current_end; // 更新前一个区间的结束位置
            }

            // 检查最后一个已占用区间之后的间隙（到全局最大值）
            long long gap_start = prev_end + 1;
            long long gap_end = global_max;
            if (gap_start <= gap_end) {
                long long gap_length = gap_end - gap_start + 1;
                if (gap_length >= required_length) {
                    long long alloc_start = gap_start;
                    long long alloc_end = alloc_start + size - 1;
                    occupied_intervals.insert({alloc_start, alloc_end}); // 占用该区间
                    return {alloc_start, alloc_end};
                }
            }

            // 未找到可用区间
            std::cout << "No available interval found.\n";
            std::cout << printOccupied() << std::endl;
            assert(false);
            return {-1, -1};
        }

        std::string printOccupied() const {
            std::stringstream ss;
            ss << "Occupied intervals:\n";
            for (const auto& interval : occupied_intervals) {
                ss << "[" << interval.first << ", " << interval.second << "], ";
            }
            return ss.str();
        }

    private:
        long long global_min;
        long long global_max;
        
        std::set<std::pair<long long, long long>> occupied_intervals;

        bool isOverlapping(long long start, long long end) const {
            auto it = occupied_intervals.lower_bound({start, 0});
            
            if (it != occupied_intervals.begin()) {
                auto prev = std::prev(it);
                if (prev->second >= start) {
                    return true;
                }
            }
            
            if (it != occupied_intervals.end() && it->first <= end) {
                return true;
            }
            
            return false;
        }
    };
}  // mudnac


#endif //MUDNACSIM_TOOLS_H
