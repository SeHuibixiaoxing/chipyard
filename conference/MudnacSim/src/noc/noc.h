#ifndef MUDNACSIM_NOC_H
#define MUDNACSIM_NOC_H


#include "net_int.h"


namespace mudnac {


    class NoC : public TickModule {
    public:
        std::vector<BaseNetworkInterface *> nodes;
        std::vector<Router *> routers;
        std::vector<uint64> nodeIdToRouterId;
        std::vector<uint64> nodeIdToPortId;
        std::vector<std::vector<uint64>> hopCountTable;

        ~NoC() = default;

        void clear() {
            nodes.clear();
            routers.clear();
            nodeIdToRouterId.clear();
            nodeIdToPortId.clear();
            hopCountTable.clear();
        }

        void addRouter(Router &router, uint64 routerId) {
            if (routerId + 1 > routers.size()) {
                routers.resize(routerId + 1, NULL);
            }
            routers[routerId] = &router;
        };

        void addNode(BaseNetworkInterface &node, uint64 nodeId, uint64 routerId, uint64 portId) {
            assert(routerId < routers.size());
            assert(routers[routerId] != NULL);
            assert(portId < routers[routerId]->ports.size());
            if (nodeId + 1 > nodes.size()) {
                nodes.resize(nodeId + 1, NULL);
                nodeIdToRouterId.resize(nodeId + 1, 0);
                nodeIdToPortId.resize(nodeId + 1, 0);
            }
            nodes[nodeId] = &node;
            nodeIdToRouterId[nodeId] = routerId;
            nodeIdToPortId[nodeId] = portId;
            node.routerPort == routers[routerId]->ports[portId];
        }

        void setHopCountTables() {
            for (auto node: nodes) {
                assert(node->hopCountTable == NULL);
                node->hopCountTable = &hopCountTable;
            }
        }

        void updateRouterConnectedPortIdList() {
            for (auto router: routers) {
                router->updateConnectedPortIdList();
            }
        }

        inline void tick() {
            for (auto node: nodes) {
                node->tick();
            }
            for (auto router: routers) {
                router->tick();
            }
        }
    };


    /** Mesh NoC
     *
     * Placement
     * (H=2, W=4)
     *      0--1--2--3
     *      |  |  |  |
     *      4--5--6--7
     *
     * Router port IDs
     * Ports 0-3 are connected to neighbor routers.
     * Other ports are connected to local nodes.
     *        2
     *      0 R 1
     *        3
     */
    class MeshNoC : public NoC {
    public:
        uint64 H = 0;
        uint64 W = 0;
        std::vector<std::vector<Router>> routerMesh;

    public:
        ~MeshNoC() = default;

        void setup(uint64 H_, uint64 W_, const RouterConfig &config) {
            clear();
            routerMesh.clear();

            assert(H_ > 0 and W_ > 0);
            H = H_;
            W = W_;
            routerMesh.resize(H);
            uint64 routerId = 0;
            for (auto &meshRow: routerMesh) {
                meshRow.reserve(W);
                for (int i = 0; i < W; i++) {
                    meshRow.emplace_back(config, routerId);
                    addRouter(meshRow.back(), routerId);
                    routerId++;
                }
            }
            connectRouterPorts();
        }

        void addNode(BaseNetworkInterface &node, uint64 nodeId, uint64 h, uint64 w, uint64 portId) {
            NoC::addNode(node, nodeId, getRouterId(h, w), portId);
        }

        void generate() {
            generateRoutingTables();
            generateHopCountTable();
            setHopCountTables();
            updateRouterConnectedPortIdList();
        }

        uint64 getRouterId(uint64 h, uint64 w) {
            assert(h < H and w < W);
            return h * W + w;
        }

        uint64 getHIdx(uint64 routerId) {
            assert(routerId < routers.size());
            return std::floor((double) routerId / W);
        }

        uint64 getWIdx(uint64 routerId) {
            assert(routerId < routers.size());
            return routerId % W;
        }

    protected:
        void connectRouterPorts() {
            for (uint64 h = 0; h < H; h++) {
                for (uint64 w = 0; w < W; w++) {
                    if (w > 0) {  // Connect the port 0 to the port 1 of the left router
                        routerMesh[h][w].ports[0] == routerMesh[h][w - 1].ports[1];
                    }
                    if (h > 0) {  // Connect the port 2 to the port 3 of the upper router
                        routerMesh[h][w].ports[2] == routerMesh[h - 1][w].ports[3];
                    }
                }
            }
        }

        void generateRoutingTables() {
            for (uint64 h = 0; h < H; h++) {
                for (uint64 w = 0; w < W; w++) {
                    Router &router = routerMesh[h][w];
                    for (uint64 nodeId = 0; nodeId < nodes.size(); nodeId++) {
                        uint64 h_ = getHIdx(nodeIdToRouterId[nodeId]);
                        uint64 w_ = getWIdx(nodeIdToRouterId[nodeId]);
                        if (w_ < w) {  // to left port
                            router.rt.set(nodeId, 0);
                        } else if (w_ > w) {  // to right port
                            router.rt.set(nodeId, 1);
                        } else {
                            if (h_ < h) {  // to up port
                                router.rt.set(nodeId, 2);
                            } else if (h_ > h) {  // to down port
                                router.rt.set(nodeId, 3);
                            } else {  // to local port
                                uint64 portId = nodeIdToPortId[nodeId];
                                assert(portId >= 4 and portId < router.ports.size());
                                router.rt.set(nodeId, portId);
                            }
                        }
                    }
                }
            }
        }

        void generateHopCountTable() {
            // Resize hopCountTable
            hopCountTable.resize(nodes.size());
            for (auto &row: hopCountTable) {
                row.resize(nodes.size(), 0);
            }
            // Compute hops
            for (uint64 srcNodeId = 0; srcNodeId < nodes.size(); srcNodeId++) {
                uint64 srcH = getHIdx(nodeIdToRouterId[srcNodeId]);
                uint64 srcW = getWIdx(nodeIdToRouterId[srcNodeId]);
                for (uint64 dstNodeId = 0; dstNodeId < nodes.size(); dstNodeId++) {
                    uint64 dstH = getHIdx(nodeIdToRouterId[dstNodeId]);
                    uint64 dstW = getWIdx(nodeIdToRouterId[dstNodeId]);
                    uint64 hops = 2 + std::abs((int64) srcH - (int64) dstH) + std::abs((int64) srcW - (int64) dstW);
                    hopCountTable[srcNodeId][dstNodeId] = hops;
                }
            }
        }

    };


}


#endif //MUDNACSIM_NOC_H
