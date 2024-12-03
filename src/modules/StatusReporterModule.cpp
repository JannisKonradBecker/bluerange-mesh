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


#include <cstdlib>


#include "ConnectionMessageTypes.h"
#include "FmTypes.h"
#include "RecordStorage.h"
#include "StatusReporterModule.h"
#include "Logger.h"
#include "Terminal.h"
#include "Utility.h"
#include "Node.h"
#include "Config.h"
#include "GlobalState.h"
#include "MeshAccessModule.h"

#include "mini-printf.h"

#if IS_ACTIVE(VIRTUAL_COM_PORT)
#include <virtual_com_port.h>
#endif //IS_ACTIVE(VIRTUAL_COM_PORT)

StatusReporterModule::StatusReporterModule()
    : Module(ModuleId::STATUS_REPORTER_MODULE, "status")
{
    isADCInitialized = false;
    this->batteryVoltageDv = 0;
    number_of_adc_channels = 0;
    //Register callbacks n' stuff
    //Save configuration to base class variables
    //sizeof configuration must be a multiple of 4 bytes
    configurationPointer = &configuration;
    configurationLength = sizeof(StatusReporterModuleConfiguration);

    //Set defaults
    ResetToDefaultConfiguration();
}

void StatusReporterModule::ResetToDefaultConfiguration()
{
    //Set default configuration values
    configuration.moduleId = moduleId;
    configuration.moduleActive = true;
    configuration.moduleVersion = STATUS_REPORTER_MODULE_CONFIG_VERSION;
    configuration.statusReportingIntervalDs = 0;
    configuration.connectionReportingIntervalDs = 0;
    configuration.nearbyReportingIntervalDs = 0;
    configuration.deviceInfoReportingIntervalDs = 0;
    configuration.liveReportingState = LiveReportTypes::LEVEL_WARN;

    CheckedMemset(nodeMeasurements, 0x00, sizeof(nodeMeasurements));

    SET_FEATURESET_CONFIGURATION(&configuration, this);
}

void StatusReporterModule::ConfigurationLoadedHandler(u8* migratableConfig, u16 migratableConfigLength)
{
    //Start the Module...

    AutoSenseModule* asMod = (AutoSenseModule*)GS->node.GetModuleById(ModuleId::AUTO_SENSE_MODULE);
    if (asMod)
    {
        asMod->RegisterDataProvider(ModuleId::STATUS_REPORTER_MODULE, this);
    }
}

void StatusReporterModule::TimerEventHandler(u16 passedTimeDs)
{
    //Peridoic Message sending does not make sense for Assets as they are not connected most of the time.
    //So instead, the asset fully relies on manual querying these messages. Other than "not making sense"
    //this can lead to issues on the Gateway if it receives a messages through a MA-Connection that has a
    //virtual partnerId as the gateway gets confused by the unknown nodeId.
    if (GET_DEVICE_TYPE() != DeviceType::ASSET)
    {
        //Device Info
        if (SHOULD_IV_TRIGGER(GS->appTimerDs + GS->appTimerRandomOffsetDs, passedTimeDs, configuration.deviceInfoReportingIntervalDs)) {
            SendDeviceInfoV2(NODE_ID_BROADCAST, 0, MessageType::MODULE_ACTION_RESPONSE);
        }
        //Status
        if (SHOULD_IV_TRIGGER(GS->appTimerDs + GS->appTimerRandomOffsetDs, passedTimeDs, configuration.statusReportingIntervalDs)) {
            SendStatus(NODE_ID_BROADCAST, 0, MessageType::MODULE_ACTION_RESPONSE);
        }
        //Connections
        if (SHOULD_IV_TRIGGER(GS->appTimerDs + GS->appTimerRandomOffsetDs, passedTimeDs, configuration.connectionReportingIntervalDs)) {
            SendAllConnections(NODE_ID_BROADCAST, 0, MessageType::MODULE_GENERAL);
        }
        //Nearby Nodes
        if (SHOULD_IV_TRIGGER(GS->appTimerDs + GS->appTimerRandomOffsetDs, passedTimeDs, configuration.nearbyReportingIntervalDs)) {
            SendNearbyNodes(NODE_ID_BROADCAST, 0, MessageType::MODULE_ACTION_RESPONSE);
        }
    }
    //BatteryMeasurement (measure short after reset and then priodically)
    if( (GS->appTimerDs < SEC_TO_DS(40) && Boardconfig->batteryAdcInputPin != -1 )
        || SHOULD_IV_TRIGGER(GS->appTimerDs, passedTimeDs, batteryMeasurementIntervalDs)){
        BatteryVoltageADC();
    }

    if (IsPeriodicTimeSendActive() != periodicTimeSendWasActivePreviousTimerEventHandler)
    {
        MeshAccessModule* maMod = (MeshAccessModule*)GS->node.GetModuleById(ModuleId::MESH_ACCESS_MODULE);
        if (maMod != nullptr) {
            maMod->UpdateMeshAccessBroadcastPacket();
        }

        periodicTimeSendWasActivePreviousTimerEventHandler = IsPeriodicTimeSendActive();
        logt("STATUSMOD", "Periodic Time Send is now: %u", (u32)periodicTimeSendWasActivePreviousTimerEventHandler);
    }

    if (IsPeriodicTimeSendActive())
    {
        timeSinceLastPeriodicTimeSendDs += passedTimeDs;
        if(IsPeriodicTimeSendActive() && 
            (timeSinceLastPeriodicTimeSendDs >= TIME_BETWEEN_PERIODIC_TIME_SENDS_DS
            || (configuration.timeReportingIntervalDs > 0 && timeSinceLastPeriodicTimeSendDs >= configuration.timeReportingIntervalDs))
            ){
            timeSinceLastPeriodicTimeSendDs = 0;

            constexpr size_t bufferSize = sizeof(ComponentMessageHeader) + sizeof(GS->timeManager.GetLocalTime());
            alignas(u32) u8 buffer[bufferSize];
            CheckedMemset(buffer, 0x00, sizeof(buffer));

            ConnPacketComponentMessage *outPacket = (ConnPacketComponentMessage*)buffer;

            outPacket->componentHeader.header.messageType = MessageType::COMPONENT_SENSE;
            outPacket->componentHeader.header.sender = GS->node.configuration.nodeId;
            outPacket->componentHeader.header.receiver = periodicTimeSendReceiver;

            outPacket->componentHeader.moduleId = ModuleId::STATUS_REPORTER_MODULE;
            outPacket->componentHeader.requestHandle = periodicTimeSendRequestHandle;
            outPacket->componentHeader.actionType = (u8)SensorMessageActionType::UNSPECIFIED;
            outPacket->componentHeader.component = (u16)StatusReporterModuleComponent::DEBUG_TIME_SENSOR;
            outPacket->componentHeader.registerAddress = (u16)StatusReporterModuleTimeRegister::TIME;

            *(decltype(GS->timeManager.GetLocalTime())*)outPacket->payload = GS->timeManager.GetLocalTime();

            GS->cm.SendMeshMessage(buffer, bufferSize);
        }
    }

    if (SHOULD_IV_TRIGGER(GS->appTimerDs, passedTimeDs, 5)) // deciseconds
    {
        GetErrorsTickHandler();
    }
}

//This method sends the node's status over the network
void StatusReporterModule::SendStatus(NodeId toNode, u8 requestHandle, MessageType messageType) const
{
    MeshConnections conn = GS->cm.GetMeshConnections(ConnectionDirection::DIRECTION_IN);
    MeshConnectionHandle inConnection;

    for (u32 i = 0; i < conn.count; i++) {
        if (conn.handles[i].IsHandshakeDone()) {
            inConnection = conn.handles[i];
        }
    }

    StatusReporterModuleStatusMessage data;

    data.batteryInfo = GetBatteryVoltage();
    data.clusterSize = GS->node.GetClusterSize();
    data.connectionLossCounter = (u8) GS->node.connectionLossCounter; //TODO: connectionlosscounter is random at the moment, and the u8 will wrap
    data.freeIn = GS->cm.freeMeshInConnections;
    data.freeOut = GS->cm.freeMeshOutConnections;
    data.inConnectionPartner = !inConnection.Exists() ? 0 : inConnection.GetPartnerId();
    data.inConnectionRSSI = !inConnection.Exists() ? 0 : inConnection.GetAverageRSSI();
    data.initializedByGateway = GS->node.initializedByGateway;

    SendModuleActionMessage(
        messageType,
        toNode,
        (u8)StatusModuleActionResponseMessages::STATUS,
        requestHandle,
        (u8*)&data,
        SIZEOF_STATUS_REPORTER_MODULE_STATUS_MESSAGE,
        false
    );
}

//Message type can be either MESSAGE_TYPE_MODULE_ACTION_RESPONSE or MESSAGE_TYPE_MODULE_GENERAL
void StatusReporterModule::SendDeviceInfoV2(NodeId toNode, u8 requestHandle, MessageType messageType) const
{
    StatusReporterModuleDeviceInfoV2Message data;

    data.manufacturerId = RamConfig->manufacturerId;
    data.deviceType = GET_DEVICE_TYPE();
    u32 deviceId[2] = {};
    FruityHal::GetDeviceIdLong(deviceId);
    deviceId[0] = Utility::SwapU32(deviceId[0]);
    deviceId[1] = Utility::SwapU32(deviceId[1]);
    CheckedMemcpy(data.chipId, deviceId, sizeof(data.chipId));
    data.serialNumberIndex = RamConfig->GetSerialNumberIndex();
    data.accessAddress = FruityHal::GetBleGapAddress();
    data.nodeVersion = GS->config.GetFruityMeshVersion();
    data.networkId = GS->node.configuration.networkId;
    data.dBmRX = Boardconfig->dBmRX;
    data.dBmTX = Conf::GetInstance().defaultDBmTX;
    data.calibratedTX = Boardconfig->calibratedTX;
    data.chipGroupId = GS->config.fwGroupIds[0];
    data.featuresetGroupId = GS->config.fwGroupIds[1];
    data.bootloaderVersion = (u16)FruityHal::GetBootloaderVersion();

    SendModuleActionMessage(
        messageType,
        toNode,
        (u8)StatusModuleActionResponseMessages::DEVICE_INFO_V2,
        requestHandle,
        (u8*)&data,
        SIZEOF_STATUS_REPORTER_MODULE_DEVICE_INFO_V2_MESSAGE,
        false
    );
}

void StatusReporterModule::SendNearbyNodes(NodeId toNode, u8 requestHandle, MessageType messageType)
{
    u16 numMeasurements = 0;
    for(int i=0; i<NUM_NODE_MEASUREMENTS; i++){
        if(nodeMeasurements[i].nodeId != 0) numMeasurements++;
    }

    u8 packetSize = (u8)(numMeasurements * 3);
    DYNAMIC_ARRAY(buffer, packetSize == 0 ? 1 : packetSize);

    u16 j = 0;
    for(int i=0; i<NUM_NODE_MEASUREMENTS; i++)
    {
        if(nodeMeasurements[i].nodeId != 0){
            NodeId sender = nodeMeasurements[i].nodeId;
            i8 rssi = (i8)(nodeMeasurements[i].rssiSum / nodeMeasurements[i].packetCount);

            CheckedMemcpy(buffer + j*3 + 0, &sender, 2);
            CheckedMemcpy(buffer + j*3 + 2, &rssi, 1);

            j++;
        }
    }

    //Clear node measurements
    CheckedMemset(nodeMeasurements, 0x00, sizeof(nodeMeasurements));

    SendModuleActionMessage(
        messageType,
        toNode,
        (u8)StatusModuleActionResponseMessages::NEARBY_NODES,
        requestHandle,
        buffer,
        packetSize,
        false
    );
}


//This method sends information about the current connections over the network
void StatusReporterModule::SendAllConnections(NodeId toNode, u8 requestHandle, MessageType messageType) const
{
    StatusReporterModuleConnectionsMessage message;
    CheckedMemset(&message, 0x00, sizeof(StatusReporterModuleConnectionsMessage));

    MeshConnections connIn = GS->cm.GetMeshConnections(ConnectionDirection::DIRECTION_IN);
    MeshConnections connOut = GS->cm.GetMeshConnections(ConnectionDirection::DIRECTION_OUT);

    u8* buffer = (u8*)&message;

    if(connIn.count > 0){
        NodeId partnerId = connIn.handles[0].GetPartnerId();
        CheckedMemcpy(buffer, &partnerId, 2);
        i8 avgRssi = connIn.handles[0].GetAverageRSSI();
        CheckedMemcpy(buffer + 2, &avgRssi, 1);
    }

    for(u32 i=0; i<connOut.count; i++){
        NodeId partnerId = connOut.handles[i].GetPartnerId();
        CheckedMemcpy(buffer + (i+1)*3, &partnerId, 2);
        i8 avgRssi = connOut.handles[i].GetAverageRSSI();
        CheckedMemcpy(buffer + (i+1)*3 + 2, &avgRssi, 1);
    }

    SendModuleActionMessage(
        MessageType::MODULE_ACTION_RESPONSE,
        NODE_ID_BROADCAST,
        (u8)StatusModuleActionResponseMessages::ALL_CONNECTIONS,
        requestHandle,
        (u8*)&message,
        SIZEOF_STATUS_REPORTER_MODULE_CONNECTIONS_MESSAGE,
        false
    );
}

void StatusReporterModule::SendAllConnectionsVerbose(NodeId toNode, u8 requestHandle, u32 connectionIndex) const
{
    const BaseConnections connections = GS->cm.GetBaseConnections(ConnectionDirection::INVALID);


    for (u32 i = 0; i < connections.count; i++)
    {
        if (connections.handles[i].Exists() && (i == connectionIndex || connectionIndex == CONNECTION_INDEX_INVALID))
        {
            const BaseConnection* c = connections.handles[i].GetConnection();

            StatusReporterModuleConnectionsVerboseMessage message;
            message.header.version = 1;
            message.header.connectionIndex = i;

            message.connection.partnerId                        = c->partnerId;
            message.connection.partnerAddress                   = c->partnerAddress;
            message.connection.connectionType                   = c->connectionType;
            message.connection.averageRssi                      = c->GetAverageRSSI();
            message.connection.connectionState                  = c->connectionState;
            message.connection.encryptionState                  = c->encryptionState;
            message.connection.connectionId                     = c->connectionId;
            message.connection.uniqueConnectionId               = c->uniqueConnectionId;
            message.connection.connectionHandle                 = c->connectionHandle;
            message.connection.direction                        = c->direction;
            message.connection.creationTimeDs                   = c->creationTimeDs;
            message.connection.handshakeStartedDs               = c->handshakeStartedDs;
            message.connection.connectionHandshakedTimestampDs  = c->connectionHandshakedTimestampDs;
            message.connection.disconnectedTimestampDs          = c->disconnectedTimestampDs;
            message.connection.droppedPackets                   = c->droppedPackets;
            message.connection.sentReliable                     = c->sentReliable;
            message.connection.sentUnreliable                   = c->sentUnreliable;
            message.connection.pendingPackets                   = c->GetPendingPackets();
            message.connection.connectionMtu                    = c->connectionMtu;
            message.connection.clusterUpdateCounter             = c->clusterUpdateCounter;
            message.connection.nextExpectedClusterUpdateCounter = c->nextExpectedClusterUpdateCounter;
            message.connection.manualPacketsSent                = c->manualPacketsSent;

            // The message is so big that we have to send the information per connection instead of all information together.
            // This is because if we send all connections in one message, we run the risk of exceeding 200 byte which is the
            // limit for messages. (Would be exceeded with 4 connections)
            SendModuleActionMessage(
                MessageType::MODULE_ACTION_RESPONSE,
                toNode,
                (u8)StatusModuleActionResponseMessages::ALL_CONNECTIONS_VERBOSE,
                requestHandle,
                (u8*)&message,
                sizeof(message),
                false
            );
        }
    }
}

void StatusReporterModule::SendRebootReason(NodeId toNode, u8 requestHandle) const
{
    SendModuleActionMessage(
        MessageType::MODULE_ACTION_RESPONSE,
        toNode,
        (u8)StatusModuleActionResponseMessages::REBOOT_REASON,
        requestHandle,
        (u8*)(GS->ramRetainStructPreviousBootPtr),
        sizeof(RamRetainStruct) - sizeof(u32), //crc32 not needed
        false
    );
}

void StatusReporterModule::SendErrors(NodeId toNode, u8 requestHandle)
{
    auto &logger = Logger::GetInstance();

    //If our time is synced we report the absolute uptime, otherwhise the relative uptime is all we know
    if(GS->timeManager.IsTimeSynced()){
        //Reports the absolute UTC boot timestamp
        u32 startTimeUtcSec = GS->timeManager.GetUtcTime() - DS_TO_SEC(GS->appTimerDs);
        logger.LogCustomError(CustomErrorTypes::INFO_UPTIME_ABSOLUTE, startTimeUtcSec);
    } else {
        //Reports the time in seconds since boot
        logger.LogCustomError(CustomErrorTypes::INFO_UPTIME_RELATIVE, GS->timeManager.GetUtcTime());
    }

#ifndef SIM_ENABLED
    //Also report how big the stack grew.
    logger.LogCustomError(CustomErrorTypes::INFO_UNUSED_STACK_BYTES, Utility::GetAmountOfUnusedStackBytes());
#endif

    // Log another error so that we know this is the last entry of the error log
    logger.LogCustomError(CustomErrorTypes::INFO_ERRORS_REQUESTED,
                          Logger::GetInstance().GetErrorLog().Size());

    // Setup the fields required by GetErrorsTickHandler for sending the entries to the node which
    // requested them.
    getErrorsCurrentDestination = toNode;
    getErrorsCurrentRequestHandle = requestHandle;
    getErrorsRemainingPops = logger.GetErrorLog().Size();
}

void StatusReporterModule::GetErrorsTickHandler()
{
    if (getErrorsCurrentDestination == NODE_ID_INVALID)
    {
        return;
    }

    if (getErrorsRemainingPops > 0)
    {
        ErrorLogEntry entry = {};

        if (Logger::GetInstance().PopErrorLogEntry(entry))
        {
            --getErrorsRemainingPops;

            StatusReporterModuleErrorLogEntryMessage data;
            data.errorType = static_cast<u8>(entry.errorType);
            data.extraInfo = entry.extraInfo;
            data.errorCode = entry.errorCode;
            data.timestamp = entry.timestamp;

            SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE, getErrorsCurrentDestination,
                    (u8) StatusModuleActionResponseMessages::ERROR_LOG_ENTRY, getErrorsCurrentRequestHandle,
                    (u8 *) &data, SIZEOF_STATUS_REPORTER_MODULE_ERROR_LOG_ENTRY_MESSAGE, false);
        }
        else
        {
            // Popping failed, meaning that the error log is empty. Someone has removed entries
            // from the error log besides us.
            getErrorsRemainingPops = 0;
        }
    }

    // Check if there are more entries to send.
    if (getErrorsRemainingPops == 0)
    {
        getErrorsCurrentDestination = NODE_ID_INVALID;
    }
}

void StatusReporterModule::SendLiveReport(LiveReportTypes type, u16 requestHandle, u32 extra, u32 extra2) const
{
    //Live reporting states are off=0, error=50, warn=100, info=150, debug=200
    if (type > configuration.liveReportingState) return;

    StatusReporterModuleLiveReportMessage data;

    data.reportType = (u8)type;
    data.extra = extra;
    data.extra2 = extra2;

    SendModuleActionMessage(
        MessageType::MODULE_GENERAL,
        NODE_ID_BROADCAST, //TODO: Could use gateway
        (u8)StatusModuleGeneralMessages::LIVE_REPORT,
        requestHandle,
        (u8*)&data,
        SIZEOF_STATUS_REPORTER_MODULE_LIVE_REPORT_MESSAGE,
        false
    );
}

#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
void StatusReporterModule::SendGatewayStatusResponse(u16 sender, u8 requestHandle, GatewayStatus status) const
{
    SendModuleActionMessage(
        MessageType::MODULE_ACTION_RESPONSE,
        sender,
        (u8)StatusModuleActionResponseMessages::GATEWAY_STATUS,
        requestHandle,
        (u8*)&status,
        sizeof(status),
        false
    );
}

void StatusReporterModule::UpdateGatewayStatus()
{
#if IS_ACTIVE(VIRTUAL_COM_PORT)
    const bool inited = isVirtualComPortInitialized();
    if (!inited) {
        gatewayStatus = GatewayStatus::USB_PORT_NOT_CONNECTED;
        return;
    }

    // When the usb port is reinitialized, the GW state is probably outdated.
    // Hence, we reset it.
    const uint32_t currentInitCounter = getVirtualComPortInitializedCounter();
    if (lastComPortInitializedCounter != currentInitCounter) {
        lastComPortInitializedCounter = currentInitCounter;
        gatewayStatus = GatewayStatus::USB_PORT_CONNECTED;
    }
#endif //IS_ACTIVE(VIRTUAL_COM_PORT)
}
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED

void StatusReporterModule::StartConnectionRSSIMeasurement(MeshConnection& connection) const{
    if (connection.IsConnected())
    {
        //Reset old values
        connection.lastReportedRssi = 0;
        connection.rssiAverageTimes1000 = 0;

        //Both possible errors are due to a disconnect and we can simply ignore them
        const ErrorType err = FruityHal::BleGapRssiStart(connection.connectionHandle, 2, 7);
        if (err != ErrorType::SUCCESS && err != ErrorType::BLE_INVALID_CONN_HANDLE)
        {
            logt("STATUSMOD", "Unexpected error for BleGapRssiStart %u", (u32)err);
        }

        logt("STATUSMOD", "RSSI measurement started for connection %u with code %u", connection.connectionId, (u32)err);
    }
}


void StatusReporterModule::GapAdvertisementReportEventHandler(const FruityHal::GapAdvertisementReportEvent & advertisementReportEvent)
{
    const u8* data = advertisementReportEvent.GetData();
    const u16 dataLength = advertisementReportEvent.GetDataLength();

    const auto packetHeader = (const AdvPacketHeader*)data;

    if (packetHeader->messageType == ManufacturerSpecificMessageType::JOIN_ME_V0)
    {
        if (packetHeader->networkId == GS->node.configuration.networkId)
        {
            if (dataLength == SIZEOF_ADV_PACKET_JOIN_ME)
            {
                const auto packet = (const AdvPacketJoinMeV0*)data;
                HandleJoinMeAdvertisement(advertisementReportEvent.GetRssi(), *packet);
            }
        }
    }
}

void StatusReporterModule::HandleJoinMeAdvertisement(const i8 rssi, const AdvPacketJoinMeV0 & packet)
{
    bool found = false;

    for (int i = 0; i < NUM_NODE_MEASUREMENTS; i++) {
        if (nodeMeasurements[i].nodeId == packet.payload.sender) {
            if (nodeMeasurements[i].packetCount == UINT16_MAX) {
                nodeMeasurements[i].packetCount = 0;
                nodeMeasurements[i].rssiSum = 0;
            }
            nodeMeasurements[i].packetCount++;
            nodeMeasurements[i].rssiSum += rssi;
            found = true;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < NUM_NODE_MEASUREMENTS; i++) {
            if (nodeMeasurements[i].nodeId == 0) {
                nodeMeasurements[i].nodeId = packet.payload.sender;
                nodeMeasurements[i].packetCount = 1;
                nodeMeasurements[i].rssiSum = rssi;

                break;
            }
        }
    }
}

#ifdef TERMINAL_ENABLED
TerminalCommandHandlerReturnType StatusReporterModule::TerminalCommandHandler(const char* commandArgs[], u8 commandArgsSize)
{
    //React on commands, return true if handled, false otherwise
    if(commandArgsSize >= 4 && TERMARGS(2, moduleName))
    {
        if(TERMARGS(0, "action"))
        {
            NodeId destinationNode = Utility::TerminalArgumentToNodeId(commandArgs[1]);

            if(TERMARGS(3, "get_status"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_STATUS,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if(TERMARGS(3,"get_device_info"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_DEVICE_INFO_V2,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if (TERMARGS(3, "get_connections"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_ALL_CONNECTIONS,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if (TERMARGS(3, "get_connections_verbose"))
            {
                StatusReporterModuleConnectionsVerboseRequestMessage message;
                CheckedMemset(&message, 0, sizeof(message));

                message.connectionIndex = commandArgsSize >= 5 ? Utility::StringToU32(commandArgs[4]) : CONNECTION_INDEX_INVALID;

                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_ALL_CONNECTIONS_VERBOSE,
                    0,
                    (u8*)&message,
                    sizeof(message),
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if(TERMARGS(3, "get_nearby"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_NEARBY_NODES,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if(TERMARGS(3,"set_init"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::SET_INITIALIZED,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if(TERMARGS(3, "keep_alive"))
            {
                // Sink routing self checking mechanism requires keep_alive message to be hops based.
                destinationNode = STATUS_REPORTER_MODULE_MAX_HOPS;
                StatusReporterModuleKeepAliveMessage msg;
                CheckedMemset(&msg, 0, sizeof(msg));

                msg.fromSink = GET_DEVICE_TYPE() == DeviceType::SINK;

                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::SET_KEEP_ALIVE,
                    0,
                    (u8*)&msg,
                    sizeof(msg),
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if(TERMARGS(3, "get_errors"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_ERRORS,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if(TERMARGS(3 ,"livereports")){
                    if (commandArgsSize < 5) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
                    //Enables or disables live reporting of connection establishments
                    u8 liveReportingState = Utility::StringToU8(commandArgs[4]);

                    SendModuleActionMessage(
                        MessageType::MODULE_TRIGGER_ACTION,
                        destinationNode,
                        (u8)StatusModuleTriggerActionMessages::SET_LIVEREPORTING,
                        0,
                        &liveReportingState,
                        1,
                        false
                    );

                    return TerminalCommandHandlerReturnType::SUCCESS;
                }
            else if(TERMARGS(3, "get_rebootreason"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_REBOOT_REASON,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if (TERMARGS(3, "set_time_reporting"))
            {
                if (commandArgsSize != 5) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
                SetTimeReportingMessage msg = {
                    Utility::StringToU16(commandArgs[4])
                };

                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::SET_TIME_REPORTING,
                    0,
                    (u8*)&msg,
                    SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
            else if (TERMARGS(3, "set_gw_status"))
            {
                if (commandArgsSize != 5) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;

                bool didError = false;

                GatewayStatus newGatewayStatus = (GatewayStatus)Utility::StringToU8(commandArgs[4], &didError);
                if (didError) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT;
                gatewayStatus = newGatewayStatus;
#if IS_ACTIVE(VIRTUAL_COM_PORT)
                lastComPortInitializedCounter = getVirtualComPortInitializedCounter();
#endif //IS_ACTIVE(VIRTUAL_COM_PORT)
                SendGatewayStatusResponse(destinationNode, 0, gatewayStatus);

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if (TERMARGS(3, "get_gw_status"))
            {
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)StatusModuleTriggerActionMessages::GET_GATEWAY_STATUS,
                    0,
                    nullptr,
                    0,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
        }
    }

    //Must be called to allow the module to get and set the config
    return Module::TerminalCommandHandler(commandArgs, commandArgsSize);
}
#endif

void StatusReporterModule::MeshMessageReceivedHandler(BaseConnection* connection, BaseConnectionSendData* sendData, ConnPacketHeader const * packetHeader)
{
    //Must call superclass for handling
    Module::MeshMessageReceivedHandler(connection, sendData, packetHeader);

    if(packetHeader->messageType == MessageType::MODULE_TRIGGER_ACTION){
        ConnPacketModule const * packet = (ConnPacketModule const *)packetHeader;

        //Check if our module is meant and we should trigger an action
        if(packet->moduleId == moduleId){

            //We were queried for our status
            StatusModuleTriggerActionMessages actionType = (StatusModuleTriggerActionMessages)packet->actionType;
            if(actionType == StatusModuleTriggerActionMessages::GET_STATUS)
            {
                SendStatus(packet->header.sender, packet->requestHandle, MessageType::MODULE_ACTION_RESPONSE);

            }
            //We were queried for our device info v2
            else if(actionType == StatusModuleTriggerActionMessages::GET_DEVICE_INFO_V2)
            {
                SendDeviceInfoV2(packet->header.sender, packet->requestHandle, MessageType::MODULE_ACTION_RESPONSE);

            }
            //We were queried for our connections
            else if(actionType == StatusModuleTriggerActionMessages::GET_ALL_CONNECTIONS)
            {
                StatusReporterModule::SendAllConnections(packetHeader->sender, packet->requestHandle, MessageType::MODULE_ACTION_RESPONSE);
            }
            //We were queried for our connections with verbosity
            else if (actionType == StatusModuleTriggerActionMessages::GET_ALL_CONNECTIONS_VERBOSE && sendData->dataLength >= sizeof(StatusReporterModuleConnectionsVerboseRequestMessage))
            {
                const StatusReporterModuleConnectionsVerboseRequestMessage* message = (const StatusReporterModuleConnectionsVerboseRequestMessage*)packet->data;
                StatusReporterModule::SendAllConnectionsVerbose(packetHeader->sender, packet->requestHandle, message->connectionIndex);
            }
            //We were queried for nearby nodes (nodes in the join_me buffer)
            else if(actionType == StatusModuleTriggerActionMessages::GET_NEARBY_NODES)
            {
                StatusReporterModule::SendNearbyNodes(packetHeader->sender, packet->requestHandle, MessageType::MODULE_ACTION_RESPONSE);
            }
            //We should set ourselves initialized
            else if(actionType == StatusModuleTriggerActionMessages::SET_INITIALIZED)
            {
                GS->node.initializedByGateway = true;

                SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE,
                    packet->header.sender,
                    (u8)StatusModuleActionResponseMessages::SET_INITIALIZED_RESULT,
                    packet->requestHandle,
                    nullptr,
                    0,
                    false
                );
            }
            else if(actionType == StatusModuleTriggerActionMessages::SET_KEEP_ALIVE)
            {
                FruityHal::FeedWatchdog();
                bool comesFromSink = false;
                if (sendData->dataLength <= SIZEOF_CONN_PACKET_MODULE)
                {
                    // Legacy support: old keep alive messages did not have additional data in them.
                    //                 For them we assume that this is coming from a sink.
                    comesFromSink = true;
                }
                else
                {
                    const StatusReporterModuleKeepAliveMessage* msg = (const StatusReporterModuleKeepAliveMessage*)&packet->data;
                    comesFromSink = msg->fromSink;
                }

                if (comesFromSink)
                {
                    GS->sinkNodeId = packet->header.sender;
                }

                if (connection != nullptr && comesFromSink)
                {
                    u16 receivedHopsToSink = STATUS_REPORTER_MODULE_MAX_HOPS - packetHeader->receiver + 1;
                    MeshConnection * meshconn = GS->cm.GetMeshConnectionToPartner(connection->partnerId).GetConnection();
                    if (meshconn != nullptr)
                    {
                        //meshconn can be nullptr if the connection for example is a mesh access connection.
                        u16 hopsToSink = meshconn->GetHopsToSink();

                        if (receivedHopsToSink != hopsToSink)
                        {
                            GS->logger.LogCustomError(CustomErrorTypes::FATAL_INCORRECT_HOPS_TO_SINK, ((u32)receivedHopsToSink << 16) | hopsToSink);
                            logt("DEBUGMOD", "FATAL receivedHopsToSink: %d", receivedHopsToSink);
                            logt("DEBUGMOD", "FATAL GetHopsToSink: %d", hopsToSink);
                            meshconn->SetHopsToSink(receivedHopsToSink);
                            SIMEXCEPTION(IncorrectHopsToSinkException);
                        }
                    }
                }
            }
            //Send back the errors
            else if(actionType == StatusModuleTriggerActionMessages::GET_ERRORS)
            {
                SendErrors(packet->header.sender, packet->requestHandle);
            }
            //Configures livereporting
            else if(actionType == StatusModuleTriggerActionMessages::SET_LIVEREPORTING)
            {
                configuration.liveReportingState = (LiveReportTypes)packet->data[0];
                logt("DEBUGMOD", "LiveReporting is now %u", (u32)configuration.liveReportingState);
            }
            //Send back the reboot reason
            else if(actionType == StatusModuleTriggerActionMessages::GET_REBOOT_REASON)
            {
                SendRebootReason(packet->header.sender, packet->requestHandle);
            }
            else if (actionType == StatusModuleTriggerActionMessages::SET_TIME_REPORTING)
            {
                DYNAMIC_ARRAY(buffer, SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE_RESPONSE);
                SetTimeReportingMessageResponse* response = (SetTimeReportingMessageResponse*) buffer;

                const SetTimeReportingMessage* timeRepMsg = (const SetTimeReportingMessage*)packet->data;
                u16 newTimeReportingIntervalDs = timeRepMsg->newTimeReportingIntervalDs;
                configuration.timeReportingIntervalDs = newTimeReportingIntervalDs;
                response->recordStorageResultCode = Utility::SaveModuleSettingsToFlash(
                    this,
                    this->configurationPointer,
                    this->configurationLength,
                    nullptr,
                    0,
                    nullptr,
                    0);

                response->timeReportingIntervalDs = newTimeReportingIntervalDs;

                SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE,
                    packet->header.sender,
                    (u8)StatusModuleActionResponseMessages::SET_TIME_REPORTING_RESULT,
                    packet->requestHandle,
                    (const u8*) response,
                    SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE_RESPONSE,
                    false
                );
            }
#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
            else if (actionType == StatusModuleTriggerActionMessages::GET_GATEWAY_STATUS)
            {
                UpdateGatewayStatus();
                SendGatewayStatusResponse(packet->header.sender, packet->requestHandle, gatewayStatus);
            }
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED

        }
    }

    //Parse Module responses
    if(packetHeader->messageType == MessageType::MODULE_ACTION_RESPONSE){

        ConnPacketModule const * packet = (ConnPacketModule const *)packetHeader;

        //Check if our module is meant and we should trigger an action
        if(packet->moduleId == moduleId)
        {
            StatusModuleActionResponseMessages actionType = (StatusModuleActionResponseMessages)packet->actionType;
            //Somebody reported its connections back
            if(actionType == StatusModuleActionResponseMessages::ALL_CONNECTIONS)
            {
                StatusReporterModuleConnectionsMessage const * packetData = (StatusReporterModuleConnectionsMessage const *) (packet->data);
                logjson("STATUSMOD", "{\"type\":\"connections\",\"nodeId\":%d,\"module\":%u,\"partners\":[%d,%d,%d,%d],\"rssiValues\":[%d,%d,%d,%d]}" SEP, packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE, packetData->partner1, packetData->partner2, packetData->partner3, packetData->partner4, packetData->rssi1, packetData->rssi2, packetData->rssi3, packetData->rssi4);
            }
            else if (actionType == StatusModuleActionResponseMessages::ALL_CONNECTIONS_VERBOSE)
            {
                StatusReporterModuleConnectionsVerboseMessage const* packetData = (StatusReporterModuleConnectionsVerboseMessage const*)(packet->data);
                bool unknownButFittingVersion = false;
                if (packetData->header.version > StatusReporterModuleConnectionsVerboseHeader::MAX_KNOWN_VERSION && sendData->dataLength >= StatusReporterModuleConnectionsVerboseMessage::SIZEOF_MAX_KNOWN_VERSION + SIZEOF_CONN_PACKET_MODULE)
                {
                    unknownButFittingVersion = true;
                }
                if (packetData->header.version == StatusReporterModuleConnectionsVerboseHeader::MAX_KNOWN_VERSION || unknownButFittingVersion)
                {
                    if (unknownButFittingVersion)
                    {
                        // The received version is unknown. It must not be used for automatic evaluation of any kind, but it may
                        // be still usable for a human reader. Thus this data is printed out but must not be trusted blindly.
                        logt("WARNING", "Received unknown connections_verbose version!");
                        logjson_partial("STATUSMOD", "{\"type\":\"connections_verbose_unknown_version\",");
                    }
                    else
                    {
                        logjson_partial("STATUSMOD", "{\"type\":\"connections_verbose\",");
                    }
                    logjson_partial("STATUSMOD", "\"nodeId\":%d,"                                 , (i32)packet->header.sender);
                    logjson_partial("STATUSMOD", "\"module\":%u,"                                 , (u32)ModuleId::STATUS_REPORTER_MODULE);
                    logjson_partial("STATUSMOD", "\"version\":%u,"                                , (u32)packetData->header.version);
                    logjson_partial("STATUSMOD", "\"connectionIndex\":%u,"                        , (u32)packetData->header.connectionIndex);
                    logjson_partial("STATUSMOD", "\"partnerId\":%u,"                              , (u32)packetData->connection.partnerId);
                    logjson_partial("STATUSMOD", "\"partnerAddress\":\"%u, [%x:%x:%x:%x:%x:%x]\",", (u32)packetData->connection.partnerAddress.addr_type,
                        (u32)packetData->connection.partnerAddress.addr[0],
                        (u32)packetData->connection.partnerAddress.addr[1],
                        (u32)packetData->connection.partnerAddress.addr[2],
                        (u32)packetData->connection.partnerAddress.addr[3],
                        (u32)packetData->connection.partnerAddress.addr[4],
                        (u32)packetData->connection.partnerAddress.addr[5]);
                    logjson_partial("STATUSMOD", "\"connectionType\":%u,"                         , (u32)packetData->connection.connectionType);
                    logjson_partial("STATUSMOD", "\"averageRssi\":%d,"                            , (i32)packetData->connection.averageRssi);
                    logjson_partial("STATUSMOD", "\"connectionState\":%u,"                        , (u32)packetData->connection.connectionState);
                    logjson_partial("STATUSMOD", "\"encryptionState\":%u,"                        , (u32)packetData->connection.encryptionState);
                    logjson_partial("STATUSMOD", "\"connectionId\":%u,"                           , (u32)packetData->connection.connectionId);
                    logjson_partial("STATUSMOD", "\"uniqueConnectionId\":%u,"                     , (u32)packetData->connection.uniqueConnectionId);
                    logjson_partial("STATUSMOD", "\"connectionHandle\":%u,"                       , (u32)packetData->connection.connectionHandle);
                    logjson_partial("STATUSMOD", "\"direction\":%u,"                              , (u32)packetData->connection.direction);
                    logjson_partial("STATUSMOD", "\"creationTimeDs\":%u,"                         , (u32)packetData->connection.creationTimeDs);
                    logjson_partial("STATUSMOD", "\"handshakeStartedDs\":%u,"                     , (u32)packetData->connection.handshakeStartedDs);
                    logjson_partial("STATUSMOD", "\"connectionHandshakedTimestampDs\":%u,"        , (u32)packetData->connection.connectionHandshakedTimestampDs);
                    logjson_partial("STATUSMOD", "\"disconnectedTimestampDs\":%u,"                , (u32)packetData->connection.disconnectedTimestampDs);
                    logjson_partial("STATUSMOD", "\"droppedPackets\":%u,"                         , (u32)packetData->connection.droppedPackets);
                    logjson_partial("STATUSMOD", "\"sentReliable\":%u,"                           , (u32)packetData->connection.sentReliable);
                    logjson_partial("STATUSMOD", "\"sentUnreliable\":%u,"                         , (u32)packetData->connection.sentUnreliable);
                    logjson_partial("STATUSMOD", "\"pendingPackets\":%u,"                         , (u32)packetData->connection.pendingPackets);
                    logjson_partial("STATUSMOD", "\"connectionMtu\":%u,"                          , (u32)packetData->connection.connectionMtu);
                    logjson_partial("STATUSMOD", "\"clusterUpdateCounter\":%u,"                   , (u32)packetData->connection.clusterUpdateCounter);
                    logjson_partial("STATUSMOD", "\"nextExpectedClusterUpdateCounter\":%u,"       , (u32)packetData->connection.nextExpectedClusterUpdateCounter);
                    logjson        ("STATUSMOD", "\"manualPacketsSent\":%u}" SEP                  , (u32)packetData->connection.manualPacketsSent);
                }
                else
                {
                    logt("ERROR", "Received unknown connections_verbose version: %u", (u32)packetData->header.version);
                }
            }
            else if(actionType == StatusModuleActionResponseMessages::DEVICE_INFO_V2)
            {
                //Print packet to console
                StatusReporterModuleDeviceInfoV2Message const * data = (StatusReporterModuleDeviceInfoV2Message const *) (packet->data);

                FruityHal::BleGapAddrBytes addr = data->accessAddress.addr;

                char serialBuffer[NODE_SERIAL_NUMBER_MAX_CHAR_LENGTH];
                Utility::GenerateBeaconSerialForIndex(data->serialNumberIndex, serialBuffer);

                logjson_partial("STATUSMOD", "{\"nodeId\":%u,\"type\":\"device_info\",\"module\":%u,", packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE);
                logjson_partial("STATUSMOD", "\"dBmRX\":%d,\"dBmTX\":%d,\"calibratedTX\":%d,", data->dBmRX, data->dBmTX, data->calibratedTX);
                logjson_partial("STATUSMOD", "\"deviceType\":%u,\"manufacturerId\":%u,", (u32)data->deviceType, data->manufacturerId);
                logjson_partial("STATUSMOD", "\"networkId\":%u,\"nodeVersion\":%u,", data->networkId, data->nodeVersion);
                logjson_partial("STATUSMOD", "\"chipId\":\"%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\",", data->chipId[0], data->chipId[1], data->chipId[2], data->chipId[3], data->chipId[4], data->chipId[5], data->chipId[6], data->chipId[7]);
                logjson_partial("STATUSMOD", "\"serialNumber\":\"%s\",\"accessAddress\":\"%02X:%02X:%02X:%02X:%02X:%02X\",", serialBuffer, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
                logjson_partial("STATUSMOD", "\"groupIds\":[%u,%u],\"blVersion\":%u", data->chipGroupId, data->featuresetGroupId, data->bootloaderVersion);
                logjson("STATUSMOD", "}" SEP);

            }
            else if(actionType == StatusModuleActionResponseMessages::STATUS)
            {
                //Print packet to console
                StatusReporterModuleStatusMessage const * data = (StatusReporterModuleStatusMessage const *) (packet->data);

                logjson_partial("STATUSMOD", "{\"nodeId\":%u,\"type\":\"status\",\"module\":%u,", packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE);
                logjson_partial("STATUSMOD", "\"batteryInfo\":%u,\"clusterSize\":%u,", data->batteryInfo, data->clusterSize);
                logjson_partial("STATUSMOD", "\"connectionLossCounter\":%u,\"freeIn\":%u,", data->connectionLossCounter, data->freeIn);
                logjson_partial("STATUSMOD", "\"freeOut\":%u,\"inConnectionPartner\":%u,", data->freeOut, data->inConnectionPartner);
                logjson_partial("STATUSMOD", "\"inConnectionRSSI\":%d, \"initialized\":%u", data->inConnectionRSSI, data->initializedByGateway);
                logjson("STATUSMOD", "}" SEP);
            }
            else if(actionType == StatusModuleActionResponseMessages::NEARBY_NODES)
            {
                //Print packet to console
                logjson_partial("STATUSMOD", "{\"nodeId\":%u,\"type\":\"nearby_nodes\",\"module\":%u,\"nodes\":[", packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE);

                u16 nodeCount = (sendData->dataLength - SIZEOF_CONN_PACKET_MODULE).GetRaw() / 3;
                bool first = true;
                for(int i=0; i<nodeCount; i++){
                    u16 nodeId;
                    i8 rssi;
                    //TODO: Find a nicer way to access unaligned data in packets
                    CheckedMemcpy(&nodeId, packet->data + i*3+0, 2);
                    CheckedMemcpy(&rssi, packet->data + i*3+2, 1);
                    if(!first){
                        logjson_partial("STATUSMOD", ",");
                    }
                    logjson_partial("STATUSMOD", "{\"nodeId\":%u,\"rssi\":%d}", nodeId, rssi);
                    first = false;
                }

                logjson("STATUSMOD", "]}" SEP);
            }
            else if(actionType == StatusModuleActionResponseMessages::SET_INITIALIZED_RESULT)
            {
                logjson("STATUSMOD", "{\"type\":\"set_init_result\",\"nodeId\":%u,\"module\":%u}" SEP, packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE);
            }
            else if(actionType == StatusModuleActionResponseMessages::ERROR_LOG_ENTRY)
            {
                StatusReporterModuleErrorLogEntryMessage const * data = (StatusReporterModuleErrorLogEntryMessage const *) (packet->data);

                logjson_partial("STATUSMOD", "{\"type\":\"error_log_entry\",\"nodeId\":%u,\"module\":%u,", packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE);

                //As the time is currently only 3 byte, use this formula to get the current unix timestamp in UTC: now()  - (now() % (2^24)) + timestamp
                logjson_partial("STATUSMOD", "\"errType\":%u,\"code\":%u,\"extra\":%u,\"time\":%u", (u32)data->errorType, data->errorCode, data->extraInfo, data->timestamp);

                logjson_partial("STATUSMOD", ",\"typeStr\":\"%s\",\"codeStr\":\"%s\"", Logger::GetErrorLogErrorType((LoggingError)data->errorType), Logger::GetErrorLogError((LoggingError)data->errorType, data->errorCode));

                logjson("STATUSMOD", "}" SEP);
            }
            else if(actionType == StatusModuleActionResponseMessages::REBOOT_REASON)
            {
                RamRetainStruct const * data = (RamRetainStruct const *) (packet->data);

                logjson_partial("STATUSMOD", "{\"type\":\"reboot_reason\",\"nodeId\":%u,\"module\":%u,", packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE);
                logjson_partial("STATUSMOD", "\"reason\":%u,\"code1\":%u,\"code2\":%u,\"code3\":%u,\"stack\":[", (u32)data->rebootReason, data->code1, data->code2, data->code3);
                for(u8 i=0; i<data->stacktraceSize; i++){
                    logjson_partial("STATUSMOD", (i < data->stacktraceSize-1) ? "%x," : "%x", data->stacktrace[i]);
                }
                logjson("STATUSMOD", "]}" SEP);
            }
            else if (actionType == StatusModuleActionResponseMessages::SET_TIME_REPORTING_RESULT)
            {
                const SetTimeReportingMessageResponse* data = (const SetTimeReportingMessageResponse*)packet->data;
                logjson(
                    "STATUSMOD", "{\"type\":\"time_reporting_state\",\"intervalDs\":%u,\"nodeId\":%u,\"module\":%u,\"code\":%u}" SEP, 
                    data->timeReportingIntervalDs, 
                    packet->header.sender, 
                    (u8)ModuleId::STATUS_REPORTER_MODULE,
                    (u8)data->recordStorageResultCode
                );
            }
#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
            else if (actionType == StatusModuleActionResponseMessages::GATEWAY_STATUS)
            {
                const GatewayStatusMessage* data = (const GatewayStatusMessage*)packet->data;
                logjson(
                    "STATUSMOD", "{\"type\":\"gw_status\",\"nodeId\":%u,\"module\":%u,\"status\":%u}" SEP, 
                    packet->header.sender, 
                    (u8)ModuleId::STATUS_REPORTER_MODULE,
                    (u8)data->gatewayStatus
                );
            }
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
        }
    }

    //Parse Module general messages
    if(packetHeader->messageType == MessageType::MODULE_GENERAL){

        ConnPacketModule const * packet = (ConnPacketModule const *)packetHeader;

        //Check if our module is meant and we should trigger an action
        if(packet->moduleId == moduleId)
        {
            StatusModuleGeneralMessages actionType = (StatusModuleGeneralMessages)packet->actionType;
            //Somebody reported its connections back
            if(actionType == StatusModuleGeneralMessages::LIVE_REPORT)
            {
                StatusReporterModuleLiveReportMessage const * packetData = (StatusReporterModuleLiveReportMessage const *) (packet->data);
                logjson("STATUSMOD", "{\"type\":\"live_report\",\"nodeId\":%d,\"module\":%u,\"code\":%u,\"extra\":%u,\"extra2\":%u}" SEP, packet->header.sender, (u8)ModuleId::STATUS_REPORTER_MODULE, packetData->reportType, packetData->extra, packetData->extra2);
            }
        }
    }

    if (packetHeader->messageType == MessageType::COMPONENT_ACT && sendData->dataLength >= SIZEOF_CONN_PACKET_COMPONENT_MESSAGE)
    {
        ConnPacketComponentMessage const * packet = (ConnPacketComponentMessage const *)packetHeader;
        if (packet->componentHeader.moduleId == moduleId)
        {
            if (packet->componentHeader.actionType == (u8)ActorMessageActionType::WRITE)
            {
                if (packet->componentHeader.component == (u16)StatusReporterModuleComponent::DEBUG_TIME_SENSOR)
                {
                    if (packet->componentHeader.registerAddress == (u16)StatusReporterModuleTimeRegister::TIME)
                    {
                        if (packet->payload[0] != 0)
                        {
                            periodicTimeSendStartTimestampDs = GS->appTimerDs;
                            timeSinceLastPeriodicTimeSendDs = TIME_BETWEEN_PERIODIC_TIME_SENDS_DS;
                            periodicTimeSendReceiver = packet->componentHeader.header.sender;
                            periodicTimeSendRequestHandle = packet->componentHeader.requestHandle;
                        }
                        else
                        {
                            periodicTimeSendStartTimestampDs = 0;
                        }
                    }
                }
            }
        }
    }
}

void StatusReporterModule::MeshConnectionChangedHandler(MeshConnection& connection)
{
    //New connection has just been made
    if(connection.HandshakeDone()){
        //TODO: Implement low and medium rssi sampling with timer handler
        //TODO: disable and enable rssi sampling on existing connections
        if(Conf::enableConnectionRSSIMeasurement){
            StartConnectionRSSIMeasurement(connection);
        }
    }
}

#define _____________BATTERY_MEASUREMENT_________________

void StatusReporterModule::AdcEventHandler()
{
    StatusReporterModule * p_statusReporterModule = (StatusReporterModule *)GS->node.GetModuleById(ModuleId::STATUS_REPORTER_MODULE);
    if (p_statusReporterModule == nullptr) return;
    p_statusReporterModule->ConvertADCtoVoltage();
    FruityHal::AdcUninit();
}

void StatusReporterModule::InitBatteryVoltageADC() {
#if IS_ACTIVE(BATTERY_MEASUREMENT)
    //Do not initialize battery checking if board does not support it
    if(Boardconfig->batteryAdcInputPin == -1 || isADCInitialized){
        return;
    }
    ErrorType error = FruityHal::AdcInit(AdcEventHandler);
    FRUITYMESH_ERROR_CHECK((u32)error);

    u32 pin = Boardconfig->batteryAdcInputPin;
    if(Boardconfig->batteryAdcInputPin == -2) 
    {
        // Battery input
        pin = 0xFF;
    }
    ErrorType err = ErrorType::SUCCESS;
#if FEATURE_AVAILABLE(ADC_INTERNAL_MEASUREMENT)
    if(Boardconfig->batteryAdcInputPin == -2) {
        err = FruityHal::AdcConfigureChannel(pin,
                                       FruityHal::AdcReference::ADC_REFERENCE_0_6V, 
                                       FruityHal::AdcResoultion::ADC_10_BIT, 
                                       FruityHal::AdcGain::ADC_GAIN_1_6);
    }
    else
    {
        err = FruityHal::AdcConfigureChannel(pin,
                                       FruityHal::AdcReference::ADC_REFERENCE_1_4_POWER_SUPPLY, 
                                       FruityHal::AdcResoultion::ADC_10_BIT, 
                                       FruityHal::AdcGain::ADC_GAIN_1_5);
    }
#else
    err = FruityHal::AdcConfigureChannel(pin, 
                                   FruityHal::AdcReference::ADC_REFERENCE_1_2V, 
                                   FruityHal::AdcResoultion::ADC_8_BIT, 
                                   FruityHal::AdcGain::ADC_GAIN_1);
#endif // FEATURE_AVAILABLE(ADC_INTERNAL_MEASUREMENT)
    if (err != ErrorType::SUCCESS)
    {
        logt("STATUS", "Failed to configure adc because %u", (u32)err);
    }
    isADCInitialized = true;
#endif
}
void StatusReporterModule::BatteryVoltageADC(){
#if IS_ACTIVE(BATTERY_MEASUREMENT)
    InitBatteryVoltageADC();
    //Check if initialization did work
    if(!isADCInitialized || Boardconfig->batteryAdcInputPin == -1) return;

#ifndef SIM_ENABLED
    if (Boardconfig->batteryAdcInputPin >= 0) {
        FruityHal::GpioConfigureOutput(Boardconfig->batteryMeasurementEnablePin);
        FruityHal::GpioPinSet(Boardconfig->batteryMeasurementEnablePin);
    }
    
    ErrorType err = FruityHal::AdcSample(*m_buffer, 1);
    FRUITYMESH_ERROR_CHECK((u32)err);

    isADCInitialized = false;
#endif
#endif
}

void StatusReporterModule::ConvertADCtoVoltage()
{
#if IS_ACTIVE(BATTERY_MEASUREMENT)
    u32 adc_sum_value = 0;
    for (u16 i = 0; i < BATTERY_SAMPLES_IN_BUFFER; i++) {
        //Buffer implemented for future use
        adc_sum_value += m_buffer[i];               //Sum all values in ADC buffer
    }

#if FEATURE_AVAILABLE(ADC_INTERNAL_MEASUREMENT)
    if (Boardconfig->batteryAdcInputPin >= 0 && Boardconfig->voltageDividerR1,Boardconfig->voltageDividerR2 > 0){
        u16 voltageDividerDv = ExternalVoltageDividerDv(Boardconfig->voltageDividerR1, Boardconfig->voltageDividerR2);
        batteryVoltageDv = FruityHal::AdcConvertSampleToDeciVoltage(adc_sum_value / BATTERY_SAMPLES_IN_BUFFER, voltageDividerDv);
    }
    else {
        batteryVoltageDv = FruityHal::AdcConvertSampleToDeciVoltage(adc_sum_value / BATTERY_SAMPLES_IN_BUFFER); //Transform the average ADC value into decivolts value
    }
#else
    batteryVoltageDv = FruityHal::AdcConvertSampleToDeciVoltage(adc_sum_value / BATTERY_SAMPLES_IN_BUFFER);
#endif // FEATURE_AVAILABLE(ADC_INTERNAL_MEASUREMENT)

#endif
}

bool StatusReporterModule::IsPeriodicTimeSendActive()
{
    return (periodicTimeSendStartTimestampDs != 0 && GS->appTimerDs < periodicTimeSendStartTimestampDs + PERIODIC_TIME_SEND_AUTOMATIC_DEACTIVATION) || configuration.timeReportingIntervalDs > 0;
}

u8 StatusReporterModule::GetBatteryVoltage() const
{
    return batteryVoltageDv;
}

u16 StatusReporterModule::ExternalVoltageDividerDv(u32 Resistor1, u32 Resistor2)
{
    u16 voltageDividerDv = u16(((double(Resistor1 + Resistor2)) / double(Resistor2)) * 10);
    return voltageDividerDv;
}

MeshAccessAuthorization StatusReporterModule::CheckMeshAccessPacketAuthorization(BaseConnectionSendData * sendData, u8 const * data, FmKeyId fmKeyId, DataDirection direction)
{
    ConnPacketHeader const * packet = (ConnPacketHeader const *)data;

    ConnPacketModule const *mod = (ConnPacketModule const *)data;
    if (mod->moduleId == moduleId)
    {
        // The Gateway uses get_status, get_device_info and set_init via the organization key.
        if (fmKeyId == FmKeyId::ORGANIZATION)
        {
            if (packet->messageType == MessageType::MODULE_TRIGGER_ACTION)
            {
                switch (mod->actionType)
                {
                case (u8)StatusModuleTriggerActionMessages::GET_ERRORS:
                case (u8)StatusModuleTriggerActionMessages::GET_REBOOT_REASON:
                case (u8)StatusModuleTriggerActionMessages::SET_KEEP_ALIVE:
                case (u8)StatusModuleTriggerActionMessages::SET_TIME_REPORTING:
                case (u8)StatusModuleTriggerActionMessages::GET_STATUS:
                case (u8)StatusModuleTriggerActionMessages::GET_DEVICE_INFO_V2:
                case (u8)StatusModuleTriggerActionMessages::SET_INITIALIZED:
#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
                case (u8)StatusModuleTriggerActionMessages::GET_GATEWAY_STATUS:
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
                    return MeshAccessAuthorization::WHITELIST;

                default:
                    return MeshAccessAuthorization::UNDETERMINED;
                }
            }
            else if (packet->messageType == MessageType::MODULE_ACTION_RESPONSE)
            {
                switch (mod->actionType)
                {
                case (u8)StatusModuleActionResponseMessages::ERROR_LOG_ENTRY:
                case (u8)StatusModuleActionResponseMessages::REBOOT_REASON:
                case (u8)StatusModuleActionResponseMessages::SET_TIME_REPORTING_RESULT:
                case (u8)StatusModuleActionResponseMessages::STATUS:
                case (u8)StatusModuleActionResponseMessages::DEVICE_INFO_V2:
                case (u8)StatusModuleActionResponseMessages::SET_INITIALIZED_RESULT:
#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
                case (u8)StatusModuleActionResponseMessages::GATEWAY_STATUS:
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
                    return MeshAccessAuthorization::WHITELIST;

                default:
                    return MeshAccessAuthorization::UNDETERMINED;
                }
            }
        }
    }

    if (packet->messageType == MessageType::COMPONENT_ACT
        || packet->messageType == MessageType::COMPONENT_SENSE)
    {
        ConnPacketComponentMessage const * packet = (ConnPacketComponentMessage const *)data;
        if (packet->componentHeader.moduleId == moduleId)
        {
            return MeshAccessAuthorization::WHITELIST;
        }
    }

    return MeshAccessAuthorization::UNDETERMINED;
}

bool StatusReporterModule::IsInterestedInMeshAccessConnection()
{
    // Only interested in MA connection if time reporting is not persistently enabled
    return periodicTimeSendStartTimestampDs != 0 && GS->appTimerDs < periodicTimeSendStartTimestampDs + PERIODIC_TIME_SEND_AUTOMATIC_DEACTIVATION;
}

CapabilityEntry StatusReporterModule::GetCapability(u32 index, bool firstCall)
{
    logt("STATUSMOD", "Status module capability request %u", (u32)index);

    if (index == 0) {
        // metadata of periodic test used for factory test with actuator SET_PERIODIC_TEST controlling IsPeriodicTimeSendActive()
        CapabilityEntry retVal;
        CheckedMemset(&retVal, 0, sizeof(retVal));
        retVal.type = CapabilityEntryType::METADATA;
        strcpy(retVal.manufacturer, "M-Way Solutions GmbH");
        strcpy(retVal.modelName, "BlueRange Node Status");
        strcpy(retVal.revision, "0");
        return retVal;
    }

    // FruityMesh version. We call it Mesh Firmware as we have two flavours: FruityMesh and BlueRange
    if (index == 1) {
        CapabilityEntry retVal;
        CheckedMemset(&retVal, 0, sizeof(retVal));
        retVal.type = CapabilityEntryType::SOFTWARE;
        strcpy(retVal.manufacturer, "BlueRange GmbH");
        strcpy(retVal.modelName, "Mesh Firmware");
        snprintf(
            retVal.revision,
            sizeof(retVal.revision),
            "%u.%u.%u",
            FM_VERSION_MAJOR,
            FM_VERSION_MINOR,
            FM_VERSION_PATCH
        );
        return retVal;
    }

    // Softdevice version in human readable form
    if (index == 2) {
        const u32 softDeviceVersion = FruityHal::GetSoftDeviceVersion();
        const u32 softDevicePatch = softDeviceVersion % 1000;
        const u32 softDeviceMinor = ((softDeviceVersion % 1000000) - softDevicePatch) / 1000;
        const u32 softDeviceMajor = softDeviceVersion / 1000000;

        CapabilityEntry retVal;
        CheckedMemset(&retVal, 0, sizeof(retVal));
        retVal.type = CapabilityEntryType::SOFTWARE;
        strcpy(retVal.manufacturer, "BlueRange GmbH");
        strcpy(retVal.modelName, "BLE Stack");
        snprintf(
            retVal.revision,
            sizeof(retVal.revision),
            "%u.%u.%u",
            softDeviceMajor,
            softDeviceMinor,
            softDevicePatch
        );
        return retVal;
            
    }

    // Our custom Bootloader called FruityLoader internally
    if (index == 3) {
        CapabilityEntry retVal;
        CheckedMemset(&retVal, 0, sizeof(retVal));
        retVal.type = CapabilityEntryType::SOFTWARE;
        strcpy(retVal.manufacturer, "BlueRange GmbH");
        strcpy(retVal.modelName, "Bootloader");
        snprintf(
            retVal.revision,
            sizeof(retVal.revision),
            "%u",
            FruityHal::GetBootloaderVersion()
        );
        return retVal;
    }

    // end of capabilities
    return Module::GetCapability(index, firstCall);
}

#if IS_ACTIVE(REGISTER_HANDLER)
RegisterGeneralChecks StatusReporterModule::GetGeneralChecks(u16 component, u16 reg, u16 length) const
{
    //We enable the RegisterHandler only for the implemented component id
    if(component == (u16)StatusReporterModuleComponent::BASIC_REGISTER_HANDLER_FUNCTIONALITY) return RegisterGeneralChecks::RGC_SUCCESS;

    return RegisterGeneralChecks::RGC_LOCATION_DISABLED;
}

void StatusReporterModule::MapRegister(u16 component, u16 reg, SupervisedValue& out, u32& persistedId)
{
    if (component == (u16)StatusReporterModuleComponent::BASIC_REGISTER_HANDLER_FUNCTIONALITY)
    {
        //Information Registers
        if (reg == REGISTER_GAP_ADDRESS_TYPE) {
            gapAddrCache = FruityHal::GetBleGapAddress();
            out.SetReadable((u8)gapAddrCache.addr_type);
        }
        if (reg == REGISTER_GAP_ADDRESS)
        {
            gapAddrCache = FruityHal::GetBleGapAddress();
            out.SetReadableRange(gapAddrCache.addr.data(), gapAddrCache.addr.size());
        }
        if(reg == REGISTER_GAP_ZERO_PAD) out.SetReadable((u8)0);
        if(reg == REGISTER_GAP_ADDRESS_STRING){
            gapAddrCache = FruityHal::GetBleGapAddress();
            sprintf((char*)gapAddrStringCache, "%02X:%02X:%02X:%02X:%02X:%02X", gapAddrCache.addr[5], gapAddrCache.addr[4], gapAddrCache.addr[3], gapAddrCache.addr[2], gapAddrCache.addr[1], gapAddrCache.addr[0]);
            out.SetReadableRange(gapAddrStringCache, sizeof(gapAddrStringCache));
        }

        //Configuration Registers
        if (reg == REGISTER_REFERENCE_MILLI_VOLT_AT_0_PERCENT) out.SetReadable(referenceMilliVolt0Percent); //TODO: Make writable once persistance is available
        if (reg == REGISTER_REFERENCE_MILLI_VOLT_AT_100_PERCENT) out.SetReadable(referenceMilliVolt100Percent); //TODO: Make writable once persistance is available

        //Data Registers
        if (reg == REGISTER_DEVICE_UPTIME) out.SetReadable(GS->appTimerDs);
        if (reg == REGISTER_ABSOLUTE_UTC_TIME) out.SetReadable(GS->timeManager.GetUtcTime());
        if (reg == REGISTER_ABSOLUTE_LOCAL_TIME) out.SetReadable(GS->timeManager.GetLocalTime());
        
        if (reg == REGISTER_CLUSTER_SIZE) out.SetReadable(GS->node.GetClusterSize());
        if (reg == REGISTER_NUM_MESH_CONNECTIONS) {
            BaseConnections conns = GS->cm.GetConnectionsOfType(ConnectionType::FRUITYMESH, ConnectionDirection::INVALID);
            u8 connCount = conns.count;
            out.SetReadable(connCount);
        }
        if (reg == REGISTER_NUM_OTHER_CONNECTIONS) {
            BaseConnections meshConns = GS->cm.GetConnectionsOfType(ConnectionType::FRUITYMESH, ConnectionDirection::INVALID);
            BaseConnections allConns = GS->cm.GetConnectionsOfType(ConnectionType::INVALID, ConnectionDirection::INVALID);
            u8 connCount = allConns.count - meshConns.count;
            out.SetReadable(connCount);
        }
        if (reg == REGISTER_MESH_CONNECTIONS_DROPPED) out.SetReadable(GS->node.connectionLossCounter);
        if (reg == REGISTER_PACKETS_SENT_RELIABLE) out.SetReadable(GS->cm.sentMeshPacketsReliable);
        if (reg == REGISTER_PACKETS_SENT_UNRELIABLE) out.SetReadable(GS->cm.sentMeshPacketsUnreliable);
        if (reg == REGISTER_PACKETS_DROPPED) out.SetReadable(GS->cm.droppedMeshPackets);
        if (reg == REGISTER_PACKETS_GENERATED) out.SetReadable(GS->cm.generatedPackets);

        if (reg == REGISTER_BATTERY_PERCENTAGE)
        {
            i32 batteryPercent = batteryVoltageDv * 100; // We start with millivolts
            batteryPercent -= referenceMilliVolt0Percent;
            batteryPercent = (batteryPercent * 100) / (i32)(referenceMilliVolt100Percent - referenceMilliVolt0Percent);
            out.SetReadable((u8)Utility::Clamp(batteryPercent, 0, 100));
        }
    }
}
#endif //IS_ACTIVE(REGISTER_HANDLER)

void StatusReporterModule::RequestData(u16 component, u16 register_, u8 length, AutoSenseModuleDataConsumer* provideTo)
{
#if IS_ACTIVE(REGISTER_HANDLER)
    DYNAMIC_ARRAY(buffer, length);
    GetRegisterValues(component, register_, buffer, length);
    provideTo->ConsumeData(ModuleId::STATUS_REPORTER_MODULE, component, register_, length, buffer);
#endif //IS_ACTIVE(REGISTER_HANDLER)
}