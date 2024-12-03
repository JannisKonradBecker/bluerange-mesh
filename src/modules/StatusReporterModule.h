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

#pragma once

#include "FlashStorage.h"
#include <Module.h>
#include <Logger.h>

#include <Terminal.h>
#include <RegisterHandler.h>
#include <AutoSenseModule.h>

constexpr u8 STATUS_REPORTER_MODULE_CONFIG_VERSION = 2;
constexpr u16 STATUS_REPORTER_MODULE_MAX_HOPS = NODE_ID_HOPS_BASE + NODE_ID_HOPS_BASE_SIZE - 1;

constexpr size_t BATTERY_SAMPLES_IN_BUFFER = 1; //Number of SAADC samples in RAM before returning a SAADC event. For low power SAADC set this constant to 1. Otherwise the EasyDMA will be enabled for an extended time which consumes high current.

enum class StatusReporterModuleComponent :u16 {
    BASIC_REGISTER_HANDLER_FUNCTIONALITY = 0,
    DEBUG_TIME_SENSOR = 0xABCD,
};

enum class StatusReporterModuleTimeRegister : u16 {
    TIME = 0x1234,
};

enum class RSSISamplingModes : u8 {
    NONE = 0,
    LOW = 1,
    MEDIUM = 2,
    HIGH = 3
};

#pragma pack(push, 1)
//Module configuration that is saved persistently
struct StatusReporterModuleConfiguration: ModuleConfiguration
{
        u16 connectionReportingIntervalDs;
        u16 statusReportingIntervalDs;
        u16 nearbyReportingIntervalDs;
        u16 deviceInfoReportingIntervalDs;
        LiveReportTypes liveReportingState;
        u16 timeReportingIntervalDs;
        //Insert more persistent config values here
};
#pragma pack(pop)

#pragma pack(push)
#pragma pack(1)
typedef struct
{
    NodeId partner1;
    i8 rssi1;
    NodeId partner2;
    i8 rssi2;
    NodeId partner3;
    i8 rssi3;
    NodeId partner4;
    i8 rssi4;

} StatusReporterModuleConnectionsMessage;
STATIC_ASSERT_SIZE(StatusReporterModuleConnectionsMessage, 12);
#pragma pack(pop)

#pragma pack(push)
#pragma pack(1)
struct StatusReporterModuleConnectionsVerboseRequestMessage
{
    u32 connectionIndex;
};
STATIC_ASSERT_SIZE(StatusReporterModuleConnectionsVerboseRequestMessage, 4);
struct StatusReporterModuleConnectionsVerboseHeader
{
    constexpr static u32 MAX_KNOWN_VERSION = 1;
    u32 version;
    u32 connectionIndex;
};
STATIC_ASSERT_SIZE(StatusReporterModuleConnectionsVerboseHeader, 8);
struct StatusReporterModuleConnectionsVerboseConnection
{
    NodeId                partnerId;
    FruityHal::BleGapAddr partnerAddress;
    ConnectionType        connectionType;
    i8                    averageRssi;
    ConnectionState       connectionState;
    EncryptionState       encryptionState;
    u8                    connectionId;
    u32                   uniqueConnectionId;
    u16                   connectionHandle;
    ConnectionDirection   direction;
    u32                   creationTimeDs;
    u32                   handshakeStartedDs;
    u32                   connectionHandshakedTimestampDs;
    u32                   disconnectedTimestampDs;
    u16                   droppedPackets;
    u16                   sentReliable;
    u16                   sentUnreliable;
    u32                   pendingPackets;
    u32                   connectionMtu;
    u8                    clusterUpdateCounter;
    u8                    nextExpectedClusterUpdateCounter;
    u8                    manualPacketsSent;
};
STATIC_ASSERT_SIZE(StatusReporterModuleConnectionsVerboseConnection, 54);
struct StatusReporterModuleConnectionsVerboseMessage
{
    constexpr static u32 SIZEOF_MAX_KNOWN_VERSION = 62;
    StatusReporterModuleConnectionsVerboseHeader header;
    StatusReporterModuleConnectionsVerboseConnection connection;
};
STATIC_ASSERT_SIZE(StatusReporterModuleConnectionsVerboseMessage, 62);

struct StatusReporterModuleKeepAliveMessage
{
    u8 fromSink : 1;
    u8 reserved : 7;
};
STATIC_ASSERT_SIZE(StatusReporterModuleKeepAliveMessage, 1);
#pragma pack(pop)

/*
 * The StatusReporterModule can respond to a number of requests for device info
 * or device status, current mesh connections, etc... it also does battery
 * measurement of the node and can be seen as a generic health service.
 */
class StatusReporterModule :
    public Module,
    public AutoSenseModuleDataProvider
{
public:

        static constexpr RSSISamplingModes connectionRSSISamplingMode = RSSISamplingModes::HIGH;
        static constexpr u32 batteryMeasurementIntervalDs = SEC_TO_DS(6*60*60);

        enum class StatusModuleTriggerActionMessages : u8
        {
            SET_LED = 0,
            GET_STATUS = 1,
            //GET_DEVICE_INFO = 2, removed as of 17.05.2019
            GET_ALL_CONNECTIONS = 3,
            GET_NEARBY_NODES = 4,
            SET_INITIALIZED = 5,
            GET_ERRORS = 6,
            GET_REBOOT_REASON = 8,
            SET_KEEP_ALIVE = 9,
            GET_DEVICE_INFO_V2 = 10,
            SET_LIVEREPORTING = 11,
            GET_ALL_CONNECTIONS_VERBOSE = 12,
            // Time reporting will periodically send the local time into the mesh.
            // The interval can be configured with the u16 payload.
            // It's stored inside the private variable "u16 timeReportingIntervalDs"
            SET_TIME_REPORTING = 13,
            GET_GATEWAY_STATUS = 14,
        };

        enum class StatusModuleActionResponseMessages : u8
        {
            SET_LED_RESULT = 0,
            STATUS = 1,
            //DEVICE_INFO = 2, removed as of 17.05.2019
            ALL_CONNECTIONS = 3,
            NEARBY_NODES = 4,
            SET_INITIALIZED_RESULT = 5,
            ERROR_LOG_ENTRY = 6,
            //DISCONNECT_REASON = 7, removed as of 21.05.2019
            REBOOT_REASON = 8,
            DEVICE_INFO_V2 = 10,
            ALL_CONNECTIONS_VERBOSE = 12,
            SET_TIME_REPORTING_RESULT = 13,
            GATEWAY_STATUS = 14,
        };

        enum class StatusModuleGeneralMessages : u8
        {
            LIVE_REPORT = 1
        };

        enum class GatewayStatus : u8 {
            // 0..9 from firmware side
            UNKNOWN = 0,
            USB_PORT_CONNECTED = 1,
            USB_PORT_NOT_CONNECTED = 2, // overwrites the other states
            // 10.. set from outside, overwrites USB_PORT_CONNECTED
            READY = 10, //Serial communication handshake done
            USB_PORT_CONNECTED_BUT_UNKNOWN_STATUS = 11, // when the gateway failed to determine current state
            READY_BUT_NO_BLUERANGE_CONNECTION = 12, // no server connection
            READY_BUT_NOT_ENROLLED = 13, // edgerouter not enrolled
            READY_BUT_NO_MESH_CONNECTION = 14, // enrolled but no other node is enrolled
            READY_AND_DFU_IN_PROGRESS = 15, // dfu in progress, higher prio than NO_MESH_CONNECTION but lower prio than NOT_ENROLLED and NO_BLUERANGE_CONNECTION
            EDGEROUTER_SHUTDOWN = 16, // edgerouter was shut down and _should_ be restarting
        };

private:

        //####### Module specific message structs (these need to be packed)
        #pragma pack(push)
        #pragma pack(1)

        typedef struct
            {
                NodeId nodeId;
                i32 rssiSum;
                u16 packetCount;
            } nodeMeasurement;

            //This message delivers non- (or not often)changing information
            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_DEVICE_INFO_V2_MESSAGE = (37);
            typedef struct
            {
                u16 manufacturerId;
                u32 serialNumberIndex;
                u8 chipId[8];
                FruityHal::BleGapAddr accessAddress;
                NetworkId networkId;
                u32 nodeVersion;
                i8 dBmRX;
                i8 dBmTX;
                DeviceType deviceType;
                i8 calibratedTX;
                NodeId chipGroupId;
                NodeId featuresetGroupId;
                u16 bootloaderVersion;

            } StatusReporterModuleDeviceInfoV2Message;
            STATIC_ASSERT_SIZE(StatusReporterModuleDeviceInfoV2Message, 37);

            //This message delivers often changing information and info about the incoming connection
            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_STATUS_MESSAGE = 9;
            typedef struct
            {
                ClusterSize clusterSize;
                NodeId inConnectionPartner;
                i8 inConnectionRSSI;
                u8 freeIn : 2;
                u8 freeOut : 6;
                u8 batteryInfo;
                u8 connectionLossCounter; //Connection losses since reboot
                u8 initializedByGateway : 1; //Set to 0 if node has been resetted and does not know its configuration

            } StatusReporterModuleStatusMessage;
            STATIC_ASSERT_SIZE(StatusReporterModuleStatusMessage, 9);

            //Used for sending error logs through the mesh
            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_ERROR_LOG_ENTRY_MESSAGE = 12;
            typedef struct
            {
                u32 errorType : 8; //Workaround necessary for packing, should be of type ErrorTypes
                u32 timestamp : 24; //This should be u32, but not enough space in that message, so it will wrap after 20 days
                u32 extraInfo;
                u32 errorCode;

            } StatusReporterModuleErrorLogEntryMessage;
            STATIC_ASSERT_SIZE(StatusReporterModuleErrorLogEntryMessage, 12);

            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_LIVE_REPORT_MESSAGE = 9;
            typedef struct
            {
                u8 reportType;
                u32 extra;
                u32 extra2;
            } StatusReporterModuleLiveReportMessage;
            STATIC_ASSERT_SIZE(StatusReporterModuleLiveReportMessage, 9);

            // for setting the time reporting interval
            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE = 2;
            struct SetTimeReportingMessage
            {
                u16 newTimeReportingIntervalDs;
            };
            STATIC_ASSERT_SIZE(SetTimeReportingMessage, SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE);

            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE_RESPONSE = 3;
            struct SetTimeReportingMessageResponse
            {
                RecordStorageResultCode recordStorageResultCode;
                u16                     timeReportingIntervalDs;
            };
            STATIC_ASSERT_SIZE(SetTimeReportingMessageResponse, SIZEOF_STATUS_REPORTER_MODULE_SET_TIME_REPORTING_MESSAGE_RESPONSE);

            static constexpr int SIZEOF_STATUS_REPORTER_MODULE_GATEWAY_STATUS_MESSAGE = 1;
            struct GatewayStatusMessage
            {
                GatewayStatus gatewayStatus;
            };
            STATIC_ASSERT_SIZE(GatewayStatusMessage, SIZEOF_STATUS_REPORTER_MODULE_GATEWAY_STATUS_MESSAGE);

        #pragma pack(pop)

        //####### Module messages end

        static constexpr int NUM_NODE_MEASUREMENTS = 20;
        nodeMeasurement nodeMeasurements[NUM_NODE_MEASUREMENTS];

        u8 batteryVoltageDv; //in decivolts
        bool isADCInitialized;
        u8 number_of_adc_channels;
        i16 m_buffer[BATTERY_SAMPLES_IN_BUFFER];

        NodeId getErrorsCurrentDestination = NODE_ID_INVALID;
        u8 getErrorsCurrentRequestHandle = 0;
        u32 getErrorsRemainingPops = 0;

#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
        GatewayStatus gatewayStatus = GatewayStatus::UNKNOWN;
        uint32_t lastComPortInitializedCounter = 0;
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED

        void SendStatus(NodeId toNode, u8 requestHandle, MessageType messageType) const;
        void SendDeviceInfoV2(NodeId toNode, u8 requestHandle, MessageType messageType) const;
        void SendNearbyNodes(NodeId toNode, u8 requestHandle, MessageType messageType);
        void SendAllConnections(NodeId toNode, u8 requestHandle, MessageType messageType) const;
        constexpr static u32 CONNECTION_INDEX_INVALID = 0xFFFFFFFF;
        void SendAllConnectionsVerbose(NodeId toNode, u8 requestHandle, u32 connectionIndex) const;

        void SendErrors(NodeId toNode, u8 requestHandle);
        void GetErrorsTickHandler();
        NO_DISCARD bool GetErrorsTrySendPendingEntry() const;

        void SendRebootReason(NodeId toNode, u8 requestHandle) const;

        void StartConnectionRSSIMeasurement(MeshConnection& connection) const;

        static void AdcEventHandler();
        void InitBatteryVoltageADC();
        void BatteryVoltageADC();

        void ConvertADCtoVoltage();

        bool periodicTimeSendWasActivePreviousTimerEventHandler = false;
        u32 periodicTimeSendStartTimestampDs = 0;
        constexpr static u32 PERIODIC_TIME_SEND_AUTOMATIC_DEACTIVATION = SEC_TO_DS(/*10 minutes*/ 10 * 60);
        constexpr static u32 TIME_BETWEEN_PERIODIC_TIME_SENDS_DS = SEC_TO_DS(5);
        u32 timeSinceLastPeriodicTimeSendDs = 0;
        NodeId periodicTimeSendReceiver = 0;
        decltype(ComponentMessageHeader::requestHandle) periodicTimeSendRequestHandle = 0;
        bool IsPeriodicTimeSendActive();

        void HandleJoinMeAdvertisement(i8 rssi, const AdvPacketJoinMeV0 & packet);

    public:

        static constexpr int SIZEOF_STATUS_REPORTER_MODULE_CONNECTIONS_MESSAGE = 12;


        DECLARE_CONFIG_AND_PACKED_STRUCT(StatusReporterModuleConfiguration);

        StatusReporterModule();

        void ConfigurationLoadedHandler(u8* migratableConfig, u16 migratableConfigLength) override final;

        void ResetToDefaultConfiguration() override final;

        void TimerEventHandler(u16 passedTimeDs) override final;

        #ifdef TERMINAL_ENABLED
        TerminalCommandHandlerReturnType TerminalCommandHandler(const char* commandArgs[], u8 commandArgsSize) override final;
        #endif

        void MeshMessageReceivedHandler(BaseConnection* connection, BaseConnectionSendData* sendData, ConnPacketHeader const * packetHeader) override final;

        void GapAdvertisementReportEventHandler(const FruityHal::GapAdvertisementReportEvent& advertisementReportEvent) override final;

        void MeshConnectionChangedHandler(MeshConnection& connection) override final;

        // You can read about the meaning of extra and extra2 at the definition of LiveReportTypes
        void SendLiveReport(LiveReportTypes type, u16 requestHandle, u32 extra, u32 extra2) const;

#if IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED
        void SendGatewayStatusResponse(u16 sender, u8 requestHandle, GatewayStatus status) const;
        void UpdateGatewayStatus();
#endif //IS_ACTIVE(ONLY_SINK_FUNCTIONALITY) || SIM_ENABLED

        u8 GetBatteryVoltage() const;

        u16 ExternalVoltageDividerDv(u32 Resistor1, u32 Resistor2);

        MeshAccessAuthorization CheckMeshAccessPacketAuthorization(BaseConnectionSendData* sendData, u8 const * data, FmKeyId fmKeyId, DataDirection direction) override final;

        bool IsInterestedInMeshAccessConnection() override final;

        // Device Capabilities
        CapabilityEntry GetCapability(u32 index, bool firstCall) override final;

#if IS_ACTIVE(REGISTER_HANDLER)
public:
    // RegisterHandler

    //Some generic default that works for some typical 2x AA rechargable batteries in row
    // Overwrite this in your board configuration if you have a different battery setup
    u32 referenceMilliVolt0Percent = 1800;
    u32 referenceMilliVolt100Percent = 2600;
    FruityHal::BleGapAddr gapAddrCache = {};
    REGISTER_STRING(gapAddrStringCache, 18);

    //Information Registers
    constexpr static u32 REGISTER_GAP_ADDRESS_TYPE = 1000; //Size 1
    constexpr static u32 REGISTER_GAP_ADDRESS = 1001; //Size 6
    constexpr static u32 REGISTER_GAP_ZERO_PAD = 1007; //Size 1 (A rather hacky workaround to let a GAP address fit into a U64)
    //Between these lines is a small bit of reserved space
    constexpr static u32 REGISTER_GAP_ADDRESS_STRING = 1010; //Size 18

    //Configuration Registers
    constexpr static u32 REGISTER_REFERENCE_MILLI_VOLT_AT_0_PERCENT = 10000; //Size 4
    constexpr static u32 REGISTER_REFERENCE_MILLI_VOLT_AT_100_PERCENT = 10004; //Size 4

    //Data Registers
    constexpr static u32 REGISTER_DEVICE_UPTIME = 30000; // Size 4
    constexpr static u32 REGISTER_ABSOLUTE_UTC_TIME = 30004; // Size 4
    constexpr static u32 REGISTER_ABSOLUTE_LOCAL_TIME = 30008; // Size 4

    constexpr static u32 REGISTER_CLUSTER_SIZE = 30100; // Size 2
    constexpr static u32 REGISTER_NUM_MESH_CONNECTIONS = 30102; // Size 1
    constexpr static u32 REGISTER_NUM_OTHER_CONNECTIONS = 30103; // Size 1
    constexpr static u32 REGISTER_MESH_CONNECTIONS_DROPPED = 30104; // Size 2
    constexpr static u32 REGISTER_PACKETS_SENT_RELIABLE = 30106; // Size 4
    constexpr static u32 REGISTER_PACKETS_SENT_UNRELIABLE = 30110; // Size 4
    constexpr static u32 REGISTER_PACKETS_DROPPED = 30114; // Size 4
    constexpr static u32 REGISTER_PACKETS_GENERATED = 30118; // Size 4

    constexpr static u32 REGISTER_BATTERY_PERCENTAGE = 30200; // Size 1

    protected:
        virtual RegisterGeneralChecks GetGeneralChecks(u16 component, u16 reg, u16 length) const override final;
        virtual void MapRegister(u16 component, u16 reg, SupervisedValue& out, u32& persistedId) override final;
#endif //IS_ACTIVE(REGISTER_HANDLER)

        // AutoSenseModuleDataProvider
        void RequestData(u16 component, u16 register_, u8 length, AutoSenseModuleDataConsumer* provideTo) override;
};

