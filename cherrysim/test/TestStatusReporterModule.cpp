////////////////////////////////////////////////////////////////////////////////
// /****************************************************************************
// **
// ** Copyright (C) 2015-2022 M-Way Solutions GmbH
// ** Contact: https://www.blureange.io/licensing
// **
// ** This file is part of the Bluerange/FruityMesh implementation
// **
// ** $BR_BEGIN_LICENSE:GPL-EXCEPT$
// ** Commercial License Usage
// ** Licensees holding valid commercial Bluerange licenses may use this file in
// ** accordance with the commercial license agreement provided with the
// ** Software or, alternatively, in accordance with the terms contained in
// ** a written agreement between them and M-Way Solutions GmbH.
// ** For licensing terms and conditions see https://www.bluerange.io/terms-conditions. For further
// ** information use the contact form at https://www.bluerange.io/contact.
// **
// ** GNU General Public License Usage
// ** Alternatively, this file may be used under the terms of the GNU
// ** General Public License version 3 as published by the Free Software
// ** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
// ** included in the packaging of this file. Please review the following
// ** information to ensure the GNU General Public License requirements will
// ** be met: https://www.gnu.org/licenses/gpl-3.0.html.
// **
// ** $BR_END_LICENSE$
// **
// ****************************************************************************/
////////////////////////////////////////////////////////////////////////////////
#include "gtest/gtest.h"

#include <HelperFunctions.h>
#include "Utility.h"
#include "CherrySimTester.h"
#include "CherrySimUtils.h"
#include "Logger.h"
#include "DebugModule.h"
#include <json.hpp>

using json = nlohmann::json;

TEST(TestStatusReporterModule, TestCommands) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);

    tester.sim->FindNodeById(1)->gs.logger.EnableTag("DEBUGMOD");
    tester.sim->FindNodeById(2)->gs.logger.EnableTag("DEBUGMOD");

    tester.SendTerminalCommand(1, "action 2 status get_status");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"nodeId\":2,\"type\":\"status\",\"module\":3");


    tester.SendTerminalCommand(1, "action 2 status get_device_info");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"nodeId\":2,\"type\":\"device_info\",\"module\":3,");

    tester.SendTerminalCommand(1, "action 2 status get_connections");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"type\":\"connections\",\"nodeId\":2,\"module\":3,\"partners\":[");

    tester.SendTerminalCommand(1, "action 2 status get_nearby");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"nodeId\":2,\"type\":\"nearby_nodes\",\"module\":3,\"nodes\":[");

    tester.SendTerminalCommand(1, "action 2 status set_init");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"type\":\"set_init_result\",\"nodeId\":2,\"module\":3}");

    //tester.SendTerminalCommand(1, "action 2 status keep_alive"); //TODO: Hard to test!
    //tester.SimulateUntilMessageReceived(10 * 1000, 1, "TODO"); 

    tester.SendTerminalCommand(1, "action 2 status get_errors");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"type\":\"error_log_entry\",\"nodeId\":2,\"module\":3,");

    tester.SendTerminalCommand(1, "action 2 status livereports 42");
    tester.SimulateUntilMessageReceived(10 * 1000, 2, "LiveReporting is now 42");

    tester.SendTerminalCommand(1, "action 2 status get_rebootreason");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"type\":\"reboot_reason\",\"nodeId\":2,\"module\":3,");
}

#if defined(PROD_SINK_NRF52) && defined(PROD_MESH_NRF52)
#ifndef GITHUB_RELEASE
TEST(TestStatusReporterModule, TestLiveReportMacAddressPart) {
    // Tests whether the mac addresse part is successfully transmitted when a node disconnects

    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 2 });
    simConfig.preDefinedPositions = { {0, 0.5,0.1}, {0.55, 0.5}, {1.1, 0.51} };
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();
    tester.sim->FindNodeById(3)->gs.logger.EnableTag("STATUSMOD");
    tester.sim->FindNodeById(1)->gs.logger.EnableTag("STATUSMOD");
    tester.sim->FindNodeById(2)->gs.logger.EnableTag("STATUSMOD");
    

    NodeEntry* disconnectingNode = &tester.sim->nodes[1];
    u32 disconnectingNodeAddress;
    CheckedMemcpy(&disconnectingNodeAddress, disconnectingNode->address.addr.data(), 4);

    u32 otherNodeAddress;
    CheckedMemcpy(&otherNodeAddress, (tester.sim->nodes + 2)->address.addr.data(), 4);

    

    char msg1[100];
    snprintf(msg1, 100, "{\"type\":\"live_report\",\"nodeId\":2,\"module\":3,\"code\":52,\"extra\":%u,", otherNodeAddress);

    char msg2[100];
    snprintf(msg2, 100, "{\"type\":\"live_report\",\"nodeId\":3,\"module\":3,\"code\":52,\"extra\":%u,", disconnectingNodeAddress);

    std::vector<SimulationMessage> messages = {
        SimulationMessage(1,msg1),
        SimulationMessage(3,msg2)
    };


    // does not always work, so we might need to retry
    int maxRetry = 5;

    RetryOrFail<TimeoutException>(
        maxRetry, [&] {
            tester.SimulateUntilClusteringDone(100 * 1000, nullptr);
            tester.SendTerminalCommand(1, "sep");
            int connHandleToDisconnect = -1;
            {
                bool foundThirdNode = false, foundGW = false;
                BaseConnection** conns = disconnectingNode->gs.cm.allConnections;
                for (size_t i = 0; i < TOTAL_NUM_CONNECTIONS; i++) {
                    if (conns[i] != nullptr && conns[i]->partnerId == 3) {
                        connHandleToDisconnect = conns[i]->connectionHandle;
                        foundThirdNode = true;
                    }
                    if (conns[i] != nullptr && conns[i]->partnerId == 1) {
                        foundGW = true;
                    }
                }
                ASSERT_TRUE(foundThirdNode && foundGW);
            }
            ASSERT_TRUE(connHandleToDisconnect != -1);
            tester.SendTerminalCommand(2, "disconnect %u", connHandleToDisconnect);
        },
        [&] {
            tester.SimulateUntilMessagesReceived(1000 * 1000, messages);
        });
    
}
#endif //GITHUB_RELEASE
#endif

TEST(TestStatusReporterModule, TestMediumPrioCommandWorksWithQueuesFull) {
    // Tests if medium prio commands (e.g. action 2 status get_status) works, even if other priorities
    // are constantly heavily used. Note that priority VITAL is not tested as this priority will always
    // be sent out first if something is available and thus would, by design, block the medium queue.

    const std::vector<DeliveryPriority> prioritiesToTest = { DeliveryPriority::LOW, DeliveryPriority::HIGH };

    for (const DeliveryPriority prio : prioritiesToTest)
    {
        CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
        SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
        simConfig.terminalId = 0;
        //testerConfig.verbose = true;

        simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
        simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
        CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
        tester.Start();

        auto stepCallback = [&]() {
            for (u32 i = 0; i < tester.sim->GetTotalNodes(); i++)
            {
                NodeIndexSetter setter(i);
                DebugModule* mod = (DebugModule*)GS->node.GetModuleById(ModuleId::DEBUG_MODULE);
                mod->SendQueueFloodMessage(prio);
            }
        };

        tester.SimulateUntilClusteringDone(100 * 1000, stepCallback);

        tester.SendTerminalCommand(1, "action 2 status get_status");
        tester.SimulateUntilMessageReceivedWithCallback(10 * 1000, 1, stepCallback, "{\"nodeId\":2,\"type\":\"status\",\"module\":3");
    }
}

TEST(TestStatusReporterModule, TestPeriodicTimeSend) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    // testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);
    tester.sim->FindNodeById(2)->gs.logger.EnableTag("STATUSMOD");

    //Send a write command
    tester.SendTerminalCommand(1, "component_act 2 3 1 0xABCD 0x1234 01 13");
    tester.SimulateUntilMessageReceived(10 * 1000, 2, "Periodic Time Send is now: 1");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"nodeId\":2,\"type\":\"component_sense\",\"module\":3,\"requestHandle\":13,\"actionType\":0,\"component\":\"0xABCD\",\"register\":\"0x1234\",\"payload\":");

    //Make sure that the status module automatically disables the periodic time send after 10 minutes.
    tester.SimulateUntilMessageReceived(10 * 60 * 1000, 2, "Periodic Time Send is now: 0");
}

TEST(TestStatusReporterModule, TestPersistentPeriodicTimeSend) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);
    tester.sim->FindNodeById(2)->gs.logger.EnableTag("STATUSMOD");

    //Send a write command
    tester.SendTerminalCommand(1, "action 2 status set_time_reporting 10");
    tester.SimulateUntilMessageReceived(1 * 1000, 1, "{\"type\":\"time_reporting_state\",\"intervalDs\":10,\"nodeId\":2,\"module\":3,\"code\":0}");
    tester.SimulateUntilMessageReceived(2 * 1000, 1, "{\"nodeId\":2,\"type\":\"component_sense\",\"module\":3,\"requestHandle\":0,\"actionType\":0,\"component\":\"0xABCD\",\"register\":\"0x1234\",\"payload\":");

    tester.SendTerminalCommand(1, "reset");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, "{\"nodeId\":2,\"type\":\"component_sense\",\"module\":3,\"requestHandle\":0,\"actionType\":0,\"component\":\"0xABCD\",\"register\":\"0x1234\",\"payload\":");
}

TEST(TestStatusReporterModule, TestGetConnectionsVerbose) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);

    //Send a write command
    tester.SendTerminalCommand(1, "action 2 status get_connections_verbose");

    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"type\":\"connections_verbose\",\"nodeId\":2,\"module\":3,\"version\":1,\"connectionIndex\":0,\"partnerId\":1,\"partnerAddress\":\"1, \\[0:0:1:0:0:0\\]\",\"connectionType\":1,\"averageRssi\":-?\\d+,\"connectionState\":4,\"encryptionState\":\\d+,\"connectionId\":\\d+,\"uniqueConnectionId\":\\d+,\"connectionHandle\":\\d+,\"direction\":\\d+,\"creationTimeDs\":\\d+,\"handshakeStartedDs\":\\d+,\"connectionHandshakedTimestampDs\":\\d+,\"disconnectedTimestampDs\":\\d+,\"droppedPackets\":\\d+,\"sentReliable\":\\d+,\"sentUnreliable\":\\d+,\"pendingPackets\":\\d+,\"connectionMtu\":\\d+,\"clusterUpdateCounter\":\\d+,\"nextExpectedClusterUpdateCounter\":\\d+,\"manualPacketsSent\":\\d+\\}");
}

#ifndef GITHUB_RELEASE
TEST(TestStatusReporterModule, TestHopsToSinkFixing) {
    Exceptions::ExceptionDisabler<IncorrectHopsToSinkException> disabler;
    Exceptions::DisableDebugBreakOnException ddboe;
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    simConfig.SetToPerfectConditions();
    //testerConfig.verbose = true;
    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 5});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(1000 * 1000);

    // Simulate some additional, fixed time to make sure that all cluster update
    // messages are sent through the mesh. These would automatically fix the hops
    // to shortest sink counter and thus would destroy the purpose of this test.
    // Manually tested this with 1000 different seed offsets (0, 1000, 2000, ...).
    // Seems to be fine.
    tester.SimulateForGivenTime(10 * 1000);

    for (int i = 1; i <= 6; i++) tester.sim->FindNodeById(i)->gs.logger.EnableTag("DEBUGMOD");

    NodeIndexSetter setter(1);
    MeshConnections inConnections = tester.sim->FindNodeById(2)->gs.cm.GetMeshConnections(ConnectionDirection::DIRECTION_IN);
    MeshConnections outConnections = tester.sim->FindNodeById(2)->gs.cm.GetMeshConnections(ConnectionDirection::DIRECTION_OUT);
    u16 invalidHops = tester.sim->GetTotalNodes() + 10; // a random value that is not possible to be correct
    u16 validHops   = tester.sim->GetTotalNodes() -  1;    // initialize to max number of hops

    // set all inConnections for node 2 to invalid and find the one with least hops to sink
    for (int i = 0; i < inConnections.count; i++) {
        NodeIndexSetter setter(1);
        u16 tempHops;
        tempHops = inConnections.handles[i].GetHopsToSink();
        if (tempHops < validHops) validHops = tempHops;
        inConnections.handles[i].SetHopsToSink(invalidHops);
    }

    // set all outConnections for node 2 to invalid and find the one with least hops to sink
    for (int i = 0; i < outConnections.count; i++) {
        NodeIndexSetter setter(1);
        outConnections.handles[i].SetHopsToSink(invalidHops);
    }

    tester.SendTerminalCommand(1, "action max_hops status keep_alive");
    tester.SimulateForGivenTime(1000 * 10);

    // get_erros will collect errors from the node but will also clear them
    tester.SendTerminalCommand(2, "action this status get_errors");
    // This must not check for the exact number in "extra" as during meshing and simulation, a different invalid amount of hops may be recorded.
    tester.SimulateUntilMessageReceived(30 * 1000, 2, "{\"type\":\"error_log_entry\",\"nodeId\":2,\"module\":3,\"errType\":2,\"code\":44,\"extra\":");

    tester.SendTerminalCommand(1, "action max_hops status keep_alive");
    tester.SimulateForGivenTime(1000 * 10);
    
    tester.SendTerminalCommand(2, "action this status get_errors");
    tester.SimulateUntilMessageReceived(10 * 1000, 2, "{\"type\":\"error_log_entry\",\"nodeId\":2,\"module\":3,");

    // We expect that incorrect hops error wont be received as hopsToSink should have been fixed together with first keep_alive message.
    {
        Exceptions::ExceptionDisabler<TimeoutException> te;
        tester.SimulateUntilMessageReceived(10 * 1000, 2, "{\"type\":\"error_log_entry\",\"nodeId\":2,\"module\":3,\"errType\":%u,\"code\":%u", (u32)LoggingError::CUSTOM, (u32)CustomErrorTypes::FATAL_INCORRECT_HOPS_TO_SINK);
        ASSERT_TRUE(tester.sim->CheckExceptionWasThrown(typeid(TimeoutException)));
    }
}
#endif //GITHUB_RELEASE


#ifndef GITHUB_RELEASE
TEST(TestStatusReporterModule, TestKeepAlive) {
    // Executes keep_alive and makes sure that no IncorrectHopsToSinkException occures.
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    simConfig.SetToPerfectConditions();
    //testerConfig.verbose = true;
    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 5});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(1000 * 1000);

    tester.SendTerminalCommand(1, "action this status keep_alive");

    tester.SimulateForGivenTime(10 * 1000); // Just simulate a little and make sure that no IncorrectHopsToSinkException occures
}
#endif //GITHUB_RELEASE

#ifndef GITHUB_RELEASE
TEST(TestStatusReporterModule, TestConnectionRssiReportingWithoutNoise) {
    //Test Rssi reporting when RssiNoise is disabled 
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    //testerConfig.verbose = true;
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.rssiNoise = false;
    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);

    tester.SimulateGivenNumberOfSteps(100);

    int rssiCalculated = (int)tester.sim->GetReceptionRssi(tester.sim->FindNodeById(1), tester.sim->FindNodeById(2));

    tester.SendTerminalCommand(1, "action 1 status get_connections");

    //Wait for the message of reported rssi
    std::vector<SimulationMessage> message = {
        SimulationMessage(1,"{\"type\":\"connections\",\"nodeId\":1,\"module\":3,\"partners\":[")
    };

    tester.SimulateUntilMessagesReceived(10 * 100, message);

    const std::string messageComplete = message[0].GetCompleteMessage();

    //parse rssi value
    auto j = json::parse(messageComplete);
    int rssisReported[4];
    rssisReported[0] = j["/rssiValues/0"_json_pointer].get<int>();
    rssisReported[1] = j["/rssiValues/1"_json_pointer].get<int>();
    rssisReported[2] = j["/rssiValues/2"_json_pointer].get<int>();
    rssisReported[3] = j["/rssiValues/3"_json_pointer].get<int>();

    int rssiReported = 0;
    for (int i = 0; i < 4; i++)
    {
        if (rssisReported[i] != 0)
        {
            rssiReported = rssisReported[i];
        }
    }

    /*Check if the reported RSSI is equal to calculated one when rssi noise is inactive*/
    if (rssiReported != rssiCalculated) {
        FAIL() << "RSSI calculated is not equal to RSSI reported";
    }
}
#endif //GITHUB_RELEASE

#ifndef GITHUB_RELEASE
TEST(TestStatusReporterModule, TestConnectionRssiReportingWithNoise) {
    //Test Rssi reporting when RssiNoise is enabled
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    //testerConfig.verbose = true;
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.rssiNoise = true;
    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    // Very long time for clustering as distance between nodes is at limit of reception
    tester.SimulateUntilClusteringDone(1000 * 1000);

    // Simulate for some time to somewhat stabilize the exponential moving average RSSI of a connection
    tester.SimulateForGivenTime(10 * 1000);

    int rssiCalculated = (int)tester.sim->GetReceptionRssi(tester.sim->FindNodeById(1), tester.sim->FindNodeById(2));

    tester.SendTerminalCommand(1, "action 1 status get_connections");

    //Wait for the message of reported rssi
    std::vector<SimulationMessage> message = {
        SimulationMessage(1,"{\"type\":\"connections\",\"nodeId\":1,\"module\":3,\"partners\":[")
    };

    tester.SimulateUntilMessagesReceived(10 * 100, message);

    const std::string messageComplete = message[0].GetCompleteMessage();

    //parse rssi value
    auto j = json::parse(messageComplete);
    int rssisReported[4];
    rssisReported[0] = j["/rssiValues/0"_json_pointer].get<int>();
    rssisReported[1] = j["/rssiValues/1"_json_pointer].get<int>();
    rssisReported[2] = j["/rssiValues/2"_json_pointer].get<int>();
    rssisReported[3] = j["/rssiValues/3"_json_pointer].get<int>();

    int rssiReported = 0;
    for (int i = 0; i < 4; i++)
    {
        if (rssisReported[i] != 0)
        {
            rssiReported = rssisReported[i];
        }
    }

    /*Check if the reported RSSI is equal to calculated one when rssi noise is inactive*/
    if (std::abs(rssiReported - rssiCalculated) > 20) {
        FAIL() << "RSSI calculated is not nearly equal to RSSI reported";
    }
}
#endif //GITHUB_RELEASE

//This test makes sure that a node will correctly report its relative or absolute uptime through the error log
TEST(TestStatusReporterModule, TestUptimeReporting) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(50 * 1000);

    //Add some more uptime so that we are sure to have a two digit uptime
    tester.SimulateForGivenTime(30 * 1000);

    //Request the error log from node 2
    tester.SendTerminalCommand(1, "action 2 status get_errors");

    //The time should be more than two digits but less than three
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"error_log_entry","nodeId":2,"module":3,"errType":2,"code":85,"extra":\d\d,"time":\d+,"typeStr":"CUSTOM","codeStr":"INFO_UPTIME_RELATIVE")");

    //Now, we set the time of the sink node and wait until it got synchronized in the mesh
    tester.SendTerminalCommand(1, "settime 10000 0");
    tester.SimulateForGivenTime(20 * 1000);

    //Now, the absolute time should be reported and should be 4 digits long
    //As the node was simulated more seconds before setting a time of 10000 than after that time
    tester.SendTerminalCommand(1, "action 2 status get_errors");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"error_log_entry","nodeId":2,"module":3,"errType":2,"code":86,"extra":\d\d\d\d,"time":\d+,"typeStr":"CUSTOM","codeStr":"INFO_UPTIME_ABSOLUTE")");
}

TEST(TestStatusReporterModule, TestCapabilitySending) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.SetToPerfectConditions();
    simConfig.terminalId = 0;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(50 * 1000);

    {
        // configure the node
        NodeIndexSetter setter(1);
        GS->logger.EnableTag("NODE");
        GS->logger.EnableTag("ERROR");
        GS->logger.EnableTag("STATUSMOD");
    }

    tester.SendTerminalCommand(1, "request_capability 2");

    // start of capabilities
    tester.SimulateUntilMessageReceived(10 * 1000, 2, "Capabilities are requested");

    // software capability of Node module
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"nodeId\":2,\"type\":\"capability_entry\",\"index\":\\d+,\"capabilityType\":2,\"manufacturer\":\"M-Way Solutions GmbH\",\"model\":\"BlueRange Node\",\"revision\":\"\\d+.\\d+.\\d+\"\\}");
    // metadata capability of Status module
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"nodeId\":2,\"type\":\"capability_entry\",\"index\":\\d+,\"capabilityType\":3,\"manufacturer\":\"M-Way Solutions GmbH\",\"model\":\"BlueRange Node Status\",\"revision\":\"0\"\\}");
    // Mesh Firmware version > 1.1.0
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"nodeId\":2,\"type\":\"capability_entry\",\"index\":\\d+,\"capabilityType\":2,\"manufacturer\":\"BlueRange GmbH\",\"model\":\"Mesh Firmware\",\"revision\":\"[1-9]\\d*\\.[1-9]\\d*\\.\\d+\"\\}");
    // Softdevice version >= 5.0.0 and < 10
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"nodeId\":2,\"type\":\"capability_entry\",\"index\":\\d+,\"capabilityType\":2,\"manufacturer\":\"BlueRange GmbH\",\"model\":\"BLE Stack\",\"revision\":\"[5-9]\\.\\d+\\.\\d+\"\\}");
    // bootloader version should be > 0
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"nodeId\":2,\"type\":\"capability_entry\",\"index\":\\d+,\"capabilityType\":2,\"manufacturer\":\"BlueRange GmbH\",\"model\":\"Bootloader\",\"revision\":\"[1-9]\\d*\"\\}");

    // end of capabilities
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, "\\{\"nodeId\":2,\"type\":\"capability_end\",\"amount\":\\d+\\}");
}


TEST(TestStatusReporterModule, TestErrorLogTimes) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.SetToPerfectConditions();
    simConfig.terminalId = 1;
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    //Simulate for a given long enough time for the two nodes to cluster
    tester.SimulateForGivenTime(30 * 1000);

    //Make sure that relative time is sent in seconds
    tester.SendTerminalCommand(1, "action 2 status get_errors");
    //Wait until we receive an error log entry with a relative time of around 30 seconds
    tester.SimulateUntilRegexMessageReceived(30 * 1000, 1, R"(error_log_entry.*time":3[0-9],.*INFO_UPTIME_RELATIVE)");

    //Make sure that absolute time is sent as UTC time in seconds, wait a bit until time is synced
    tester.SendTerminalCommand(1, "settime 10000 120");
    tester.SimulateForGivenTime(30 * 1000);
    tester.SendTerminalCommand(1, "action 2 status get_errors");
    tester.SimulateUntilRegexMessageReceived(30 * 1000, 1, R"(error_log_entry.*time":100[0-9]{2},.*INFO_UPTIME_ABSOLUTE)");
}


#if defined(PROD_SINK_NRF52) && defined(PROD_ASSET_NRF52)
TEST(TestStatusReporterModule, TestActionsOnAssetOverMeshAccessSerialConnectWithOrgaKey)
{
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    //testerConfig.verbose               = true;
    SimConfiguration simConfig         = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId               = 0;
    simConfig.defaultNetworkId         = 0;
    simConfig.preDefinedPositions      = {{0.1, 0.1}, {0.2, 0.1}, {0.3, 0.1}};
    simConfig.nodeConfigName.insert({"prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({"prod_mesh_nrf52", 1});
    simConfig.nodeConfigName.insert({"prod_asset_nrf52", 1});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateGivenNumberOfSteps(10);

    tester.SendTerminalCommand(
        1, "action this enroll basic BBBBB 1 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 "
           "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 "
           "01:00:00:00:01:00:00:00:01:00:00:00:01:00:00:00 10 0 0");
    tester.SendTerminalCommand(
        2, "action this enroll basic BBBBC 2 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 "
           "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 "
           "02:00:00:00:02:00:00:00:02:00:00:00:02:00:00:00 10 0 0");
    tester.SendTerminalCommand(
        3, "action this enroll basic BBBBD 33000 20000 22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 "
           "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 "
           "03:00:00:00:03:00:00:00:03:00:00:00:03:00:00:00 10 0 0");

    tester.SimulateUntilMessageReceived(100 * 1000, 1, "clusterSize\":2"); // Wait until the nodes have clustered.

    RetryOrFail<TimeoutException>(
        32, [&] {
            // Initiate a connection from the sink on the mesh node to the asset tag using the organization key.
            tester.SendTerminalCommand(
                1, "action 2 ma serial_connect BBBBD 4 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 33000 20 13");
        },
        [&] {
            tester.SimulateUntilMessageReceived(
                10 * 1000, 1,
                R"({"type":"serial_connect_response","module":10,"nodeId":2,)"
                R"("requestHandle":13,"code":0,"partnerId":33000})");
        });

    tester.SendTerminalCommand(1, "action 33000 status get_status");
    // Wait for the response on the sink.
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":33000,"type":"status","module":3,)");

    tester.SendTerminalCommand(1, "action 33000 status get_device_info");
    // Wait for the response on the sink.
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":33000,"type":"device_info","module":3,)");

    tester.SendTerminalCommand(1, "action 33000 status set_init");
    // Wait for the response on the sink.
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"type":"set_init_result","nodeId":33000,"module":3})");
}
#endif

TEST(TestStatusReporterModule, TestNearbyNodesReportsOnlyWithSameNetworkId)
{
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    //testerConfig.verbose               = true;
    SimConfiguration simConfig         = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId               = 0;
    simConfig.defaultNetworkId         = 0;
    simConfig.preDefinedPositions      = {{0.1, 0.1}, {0.2, 0.1}, {0.3, 0.1}};
    simConfig.nodeConfigName.insert({"prod_sink_nrf52", 1});
    simConfig.nodeConfigName.insert({"prod_mesh_nrf52", 2});
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateGivenNumberOfSteps(10);

    tester.SendTerminalCommand(
        1, "action this enroll basic BBBBB 1 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 "
        "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 "
        "01:00:00:00:01:00:00:00:01:00:00:00:01:00:00:00 10 0 0");
    tester.SendTerminalCommand(
        2, "action this enroll basic BBBBC 2 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 "
        "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 "
        "02:00:00:00:02:00:00:00:02:00:00:00:02:00:00:00 10 0 0");
    tester.SendTerminalCommand(
        3, "action this enroll basic BBBBD 3 20000 22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 "
        "22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 "
        "03:00:00:00:03:00:00:00:03:00:00:00:03:00:00:00 10 0 0");

    tester.SimulateUntilMessageReceived(100 * 1000, 1, "clusterSize\":2"); // Wait until the nodes have clustered.

    for (unsigned trial = 0; trial < 10; ++trial)
    {
        tester.SimulateForGivenTime(5000);

        RetryOrFail<TimeoutException>(
            32, [&] {
                tester.SendTerminalCommand(1, "action 2 status get_nearby");
            },
            [&] {
                tester.SimulateUntilRegexMessageReceived(1000, 1, R"("nodeId":2,"type":"nearby_nodes","module":[0-9]+,"nodes":\[.*"nodeId":1,)");
            });

        {
            Exceptions::ExceptionDisabler<TimeoutException> te;

            tester.SendTerminalCommand(2, "action 2 status get_nearby");
            tester.SimulateUntilRegexMessageReceived(1000, 2, R"("nodeId":2,"type":"nearby_nodes","module":[0-9]+,"nodes":\[.*"nodeId":3,)");
            ASSERT_TRUE(tester.sim->CheckExceptionWasThrown(typeid(TimeoutException)));
        }
    }
}

TEST(TestStatusReporterModule, TestRegisters) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    testerConfig.verbose = false;
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.nodeConfigName.insert({"prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);

    //GAP_ADDRESS_TYPE
    tester.SendTerminalCommand(1, "component_act this 3 2 0 1000 01");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x03E8","payload":"AQ=="})"); // 01 (Random static)

    //GAP_ADDRESS
    tester.SendTerminalCommand(1, "component_act this 3 2 0 1001 06");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x03E9","payload":"AAABAAAA"})"); // 00:00:01:00:00:00

    //GAP_ADDRESS_TYPE as U64 including type and padding
    tester.SendTerminalCommand(1, "component_act this 3 2 0 1000 08");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x03E8","payload":"AQAAAQAAAAA="})"); // 01:00:00:01:00:00:00:00
    
    //Just so we can cross-check the string representation
    tester.SendTerminalCommand(1, "action this status get_device_info");
    tester.SimulateGivenNumberOfSteps(1);

    //GAP_ADDRESS_STRING
    tester.SendTerminalCommand(1, "component_act this 3 2 0 1010 12");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x03F2","payload":"MDA6MDA6MDA6MDE6MDA6MDAA"})"); // "00:00:00:01:00:00\0" (typically reversed when shown as string)

    //REFERENCE_MILLI_VOLT_AT_0_PERCENT
    tester.SendTerminalCommand(1, "component_act this 3 2 0 10000 04");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x2710","payload":"CAcAAA=="})"); // 1800 by default

    //TODO: Add write test once persistance works

    //REFERENCE_MILLI_VOLT_AT_100_PERCENT
    tester.SendTerminalCommand(1, "component_act this 3 2 0 10004 04");
    tester.SimulateUntilMessageReceived(10 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x2714","payload":"KAoAAA=="})"); // 2600 by default

    //TODO: Add write test once persistance works

    //DEVICE_UPTIME
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30000 04");
    // TODO: Doesn't actually check that a correct value is returned, as that would be extremely sensitive to any change of timeings and would often break this test.
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7530","payload":)");

    //ABSOLUTE_UTC_TIME (not set)
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30004 04");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7534","payload":"AAAAAA=="})"); //Reports 0 if time not synced

    //ABSOLUTE_LOCAL_TIME (not set)
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30008 04");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7538","payload":"AAAAAA=="})"); //Reports 0 if time not synced

    tester.SendTerminalCommand(1, "settime 100000000 0");
    tester.SimulateForGivenTime(1 * 1000);

    //ABSOLUTE_UTC_TIME (set to >100000000)
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30004 04");
    tester.SimulateUntilRegexMessageReceived(100 * 1000, 1, R"("nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7534","payload":".*Q==")");

    //ABSOLUTE_LOCAL_TIME (set to >100000000)
    //TODO: This does not test that the timezone works
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30008 04");
    tester.SimulateUntilRegexMessageReceived(100 * 1000, 1, R"("nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7538","payload":".*Q==")");

    //CLUSTER_SIZE
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30100 02");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7594","payload":"AQA="})"); //Only 1 node in the cluster

    //NUM_MESH_CONNECTIONS
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30102 01");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7596","payload":"AA=="})");

    //NUM_OTHER_CONNECTIONS
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30103 01");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7597","payload":"AA=="})");

    //MESH_CONNECTIONS_DROPPED
    tester.SendTerminalCommand(1, "component_act this 3 read 0 30104 02");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x7598","payload":"AAA="})");

    //PACKETS_SENT_RELIABLE
    tester.SendTerminalCommand(1, "component_act this 3 read 0 30106 04");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x759A","payload":"AAAAAA=="})");

    //PACKETS_SENT_UNRELIABLE
    tester.SendTerminalCommand(1, "component_act this 3 read 0 30110 04");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x759E","payload":"AAAAAA=="})");

    //PACKETS_DROPPED
    tester.SendTerminalCommand(1, "component_act this 3 read 0 30114 04");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x75A2","payload":"AAAAAA=="})");

    //PACKETS_GENERATED
    tester.SendTerminalCommand(1, "component_act this 3 read 0 30118 04");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x75A6","payload":)"); //The exact amount varies so it cannot be easily tested

    //BATTERY_PERCENTAGE
    tester.SendTerminalCommand(1, "component_act this 3 2 0 30200 01");
    tester.SimulateUntilMessageReceived(100 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":2,"component":"0x0000","register":"0x75F8","payload":"AA=="})"); //Measurement is 0V by default

}

#ifndef GITHUB_RELEASE
TEST(TestStatusReporterModule, TestRegistersAutoSense) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    //testerConfig.verbose = true;
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    // Configure AutoSense to query the MAC Address repeatedly
    AutoSenseTableEntryBuilder ast = {};
    ast.entry.destNodeId = 1;
    ast.entry.moduleId = Utility::GetWrappedModuleId(ModuleId::STATUS_REPORTER_MODULE);
    ast.entry.component = 0;
    ast.entry.register_ = 1000;
    ast.entry.length = 8;
    ast.entry.requestHandle = 0;
    ast.entry.dataType = DataTypeDescriptor::RAW;
    ast.entry.pollingIvDs = SEC_TO_DS(10);
    ast.entry.reportingIvDs = SEC_TO_DS(10);
    ast.entry.reportFunction = AutoSenseFunction::LAST;

    //Set AutoSense to automatic reporting to query the MAC address
    tester.SendTerminalCommand(1, "action this autosense set_autosense_entry 0 0 %s", ast.getEntry().data());
    tester.SimulateUntilMessageReceived(20 * 1000, 1, R"({"nodeId":1,"type":"component_sense","module":3,"requestHandle":0,"actionType":0,"component":"0x0000","register":"0x03E8","payload":"AQAAAQAAAAA="})");
}
#endif //GITHUB_RELEASE

#if defined(PROD_SINK_NRF52) && defined(PROD_ASSET_NRF52)
//This test makes sure that the error logs can be queried from mesh nodes and asset tags
TEST(TestStatusReporterModule, TestErrorLogQuerying) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    simConfig.mapWidthInMeters = 10;
    simConfig.mapHeightInMeters = 10;
    simConfig.SetToPerfectConditions();
    //testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_mesh_nrf52", 1 });
    simConfig.nodeConfigName.insert({ "prod_asset_nrf52", 2 });



    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    //action [nodeId] enroll basic [serialNumber] [newNodeId] [newNetworkId] {newNetworkKey} {newUserBaseKey} {newOrganizationKey} {nodeKey} {timeoutSec} {enrollOnlyIfUnenrolled} {requestHandle}
    tester.SendTerminalCommand(1, "action 0 enroll basic BBBBB 1 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 01:00:00:00:01:00:00:00:01:00:00:00:01:00:00:00 10 0 0");
    tester.SendTerminalCommand(2, "action 0 enroll basic BBBBC 2 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 02:00:00:00:02:00:00:00:02:00:00:00:02:00:00:00 10 0 0");
    tester.SendTerminalCommand(3, "action 0 enroll basic BBBBD 33000 10000 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 03:00:00:00:03:00:00:00:03:00:00:00:03:00:00:00 10 0 0");

    // second asset gets a different network but same orga
    tester.SendTerminalCommand(4, "action 0 enroll basic BBBBF 33001 20000 99:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 22:22:22:22:22:22:22:22:22:22:22:22:22:22:22:22 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 04:00:00:00:04:00:00:00:04:00:00:00:04:00:00:00 10 0 0");


    tester.SimulateUntilClusteringDone(50 * 1000);

    //////// Node 2 (same network)
    //Request the error log from node 2
    tester.SendTerminalCommand(1, "action 2 status get_errors");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"error_log_entry","nodeId":2,"module":3,"errType":2,.*"typeStr":"CUSTOM","codeStr":"INFO_ERRORS_REQUESTED")");



    //////// Node 3 (same network)
    //Establish a mesh access connection as nodeId=3 is an asset using the network key 
    RetryOrFail<TimeoutException>(
        2, [&] {
            // action [nodeId] ma serial_connect [serial number] [keyId] [key] [nodeId_of_partner_after_connect] [initial_keep_alive] {requestHandle=0} {bleAddress=""} {forceMode=0}
            tester.SendTerminalCommand(1, "action 2 ma serial_connect BBBBD 2 11:11:11:11:11:11:11:11:11:11:11:11:11:11:11:11 33010 60 12");
        },
        [&] {
            tester.SimulateUntilMessageReceived(100 * 1000, 1, "{\"type\":\"serial_connect_response\",\"module\":10,\"nodeId\":2,\"requestHandle\":12,\"code\":0,\"partnerId\":33010}");
        });

    //Request the error log from node 3 using the partnerId
    tester.SendTerminalCommand(1, "action 33010 status get_errors");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"error_log_entry","nodeId":33010,"module":3,"errType":2,.*"typeStr":"CUSTOM","codeStr":"INFO_ERRORS_REQUESTED")");



    //////// Node 4 (different network)
    //Establish a mesh access connection as nodeId=4 is an asset using the orga key 
    RetryOrFail<TimeoutException>(
        2, [&] {
            // action [nodeId] ma serial_connect [serial number] [keyId] [key] [nodeId_of_partner_after_connect] [initial_keep_alive] {requestHandle=0} {bleAddress=""} {forceMode=0}
            tester.SendTerminalCommand(1, "action 2 ma serial_connect BBBBF 4 33:33:33:33:33:33:33:33:33:33:33:33:33:33:33:33 33011 60 12");
        },
        [&] {
            tester.SimulateUntilMessageReceived(100 * 1000, 1, "{\"type\":\"serial_connect_response\",\"module\":10,\"nodeId\":2,\"requestHandle\":12,\"code\":0,\"partnerId\":33011}");
        });

    //Request the error log from node 3 using the partnerId
    tester.SendTerminalCommand(1, "action 33011 status get_errors");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"error_log_entry","nodeId":33011,"module":3,"errType":2,.*"typeStr":"CUSTOM","codeStr":"INFO_ERRORS_REQUESTED")");
}
#endif

TEST(TestStatusReporterModule, TestGatewayStatus) {
    CherrySimTesterConfig testerConfig = CherrySimTester::CreateDefaultTesterConfiguration();
    SimConfiguration simConfig = CherrySimTester::CreateDefaultSimConfiguration();
    simConfig.terminalId = 0;
    // testerConfig.verbose = true;

    simConfig.nodeConfigName.insert({ "prod_sink_nrf52", 1 });
    CherrySimTester tester = CherrySimTester(testerConfig, simConfig);
    tester.Start();

    tester.SimulateUntilClusteringDone(100 * 1000);

    // Should be unknown since we compile without the virtual comport enabled flag
    // should be NO_SERIAL_CONNECTION (1) if virtual comport is enabled (e.g. in real life)
    tester.SendTerminalCommand(1, "action 1 status get_gw_status");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"gw_status","nodeId":\d+,"module":\d+,"status":0)");

    tester.SendTerminalCommand(1, "action 1 status set_gw_status 11");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"gw_status","nodeId":\d+,"module":\d+,"status":11)");
    tester.SendTerminalCommand(1, "action 1 status get_gw_status");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"gw_status","nodeId":\d+,"module":\d+,"status":11)");

    //Non-enum values are allowed
    tester.SendTerminalCommand(1, "action 1 status set_gw_status 22");
    tester.SimulateUntilRegexMessageReceived(10 * 1000, 1, R"("type":"gw_status","nodeId":\d+,"module":\d+,"status":22)");
}