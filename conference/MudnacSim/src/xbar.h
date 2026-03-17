#ifndef MUDNACSIM_XBAR_H
#define MUDNACSIM_XBAR_H


#include "tools.h"


namespace mudnac {

    /**
     * @brief A crossbar module. Connect multiple master ports to multiple slave ports.
     * 
     */
    class XBar : public TickModule {
    public:
        std::vector<Port*> portVec;
        std::vector<int> validPortGlobalIds;

        XBar() : portVec (PortIDManager::getPortIDNum(), NULL){}

        void connectPort(uint portId, Port& port) {
            assert(portId <= PortIDManager::getMaxPortID());
            assert(port.notNull());
            assert(portVec[portId] == NULL);
            portVec[portId] = &port;
            validPortGlobalIds.push_back(portId);
        }

        inline void tick() {            
            for (auto portGlobalId: validPortGlobalIds) {
                auto port = portVec[portGlobalId];
                while (not port->output->empty()) {
                    auto msg = port->output->front();
                    port->output->pop_front();
                    if (msg->isMulticastRespMsg()) {
                        for (Message *unicastMsg: msg->multicastMsgs) { // mujlticast messages 
                            assert(unicastMsg->slavePortId <= PortIDManager::getMaxPortID());
                            portVec[unicastMsg->masterPortId]->input->push_back(unicastMsg);
                        }
                        delete msg;
                    } else {
                        if(PortIDManager::isMaster(portGlobalId)) {
                            assert(msg->slavePortId <= PortIDManager::getMaxPortID());
                            portVec[msg->slavePortId]->input->push_back(msg);                            
                        } else {
                            assert(msg->masterPortId <= PortIDManager::getMaxPortID());
                            portVec[msg->masterPortId]->input->push_back(msg);
                        }
                    }
                }
            }            
        }
    };


}


#endif //MUDNACSIM_XBAR_H
