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


#include "FmTypes.h"
#include "FruityHal.h"
#include <IoModule.h>
#include <Logger.h>
#include <Utility.h>
#include <GlobalState.h>
#include <Node.h>
#include <AutoSenseModule.h>

constexpr u8 IO_MODULE_CONFIG_VERSION = 1;

constexpr u32 HOLD_TIME_DURATION_DS = 5;

IoModule::IoModule()
    : Module(ModuleId::IO_MODULE, "io")
{
    //Save configuration to base class variables
    //sizeof configuration must be a multiple of 4 bytes
    configurationPointer = &configuration;
    configurationLength = sizeof(IoModuleConfiguration);

    //Set defaults
    ResetToDefaultConfiguration();
}

void IoModule::ResetToDefaultConfiguration()
{
    //Set default configuration values
    configuration.moduleId = moduleId;
    configuration.moduleActive = true;
    configuration.moduleVersion = IO_MODULE_CONFIG_VERSION;

    //Set additional config values...
    configuration.ledMode = Conf::GetInstance().defaultLedMode;

    SET_FEATURESET_CONFIGURATION(&configuration, this);
}

void IoModule::ConfigurationLoadedHandler(u8* migratableConfig, u16 migratableConfigLength)
{
    //Do additional initialization upon loading the config
    currentLedMode = configuration.ledMode;

    //Start the Module...
    //Configure vibration motor and buzzer pin output pins
    vibrationPins.pinsetIdentifier = PinsetIdentifier::VIBRATION;
    Boardconfig->getCustomPinset(&vibrationPins);
    if(vibrationPins.vibrationPin != -1) FruityHal::GpioConfigureOutput(vibrationPins.vibrationPin);

    buzzerPins.pinsetIdentifier = PinsetIdentifier::BUZZER;
    Boardconfig->getCustomPinset(&buzzerPins);
    if(buzzerPins.buzzerPin != -1) FruityHal::GpioConfigureOutput(buzzerPins.buzzerPin);

    //We only perform these actions when starting the module for the first time
    //as we would otherwhise need to unregister some interrupts first
    if (!moduleStarted) {
        //Configure the Digital Output Pins
        for (u32 i = 0; i < numDigitalOutPinSettings; i++) {
            FruityHal::GpioConfigureOutput(digitalOutPinSettings[i].pin);
            //Set to off by default
            if (digitalOutPinSettings[i].activeHigh) {
                FruityHal::GpioPinClear(digitalOutPinSettings[i].pin);
            }
            else {
                FruityHal::GpioPinSet(digitalOutPinSettings[i].pin);
            }
        }

        //Configure the Digital Input Pins
        for (u32 i = 0; i < numDigitalInPinSettings; i++) {
            FruityHal::GpioPullMode pullMode = digitalInPinSettings[i].activeHigh ? FruityHal::GpioPullMode::GPIO_PIN_PULLDOWN : FruityHal::GpioPullMode::GPIO_PIN_PULLUP;
            FruityHal::GpioTransition gpioTransition = digitalInPinSettings[i].activeHigh ? FruityHal::GpioTransition::GPIO_TRANSITION_LOW_TO_HIGH : FruityHal::GpioTransition::GPIO_TRANSITION_HIGH_TO_LOW;

            if (digitalInPinSettings[i].readMode == DigitalInReadMode::INTERRUPT) {
                ErrorType err = FruityHal::GpioConfigureInterrupt(digitalInPinSettings[i].pin, pullMode, gpioTransition, GpioHandler);
                if (err != ErrorType::SUCCESS) logt("ERROR", "Failed to initialize digital inputs: %u", (u32)err);
            }
            else {
                FruityHal::GpioConfigureInput(digitalInPinSettings[i].pin, pullMode);
            }
        }

        //Register our module as a data provider for AutoSense
        AutoSenseModule* asMod = (AutoSenseModule*)GS->node.GetModuleById(ModuleId::AUTO_SENSE_MODULE);
        if (asMod)
        {
            asMod->RegisterDataProvider(ModuleId::IO_MODULE, this);
        }
    }
}



void IoModule::TimerEventHandler(u16 passedTimeDs)
{
    //LED control and Identification
    if (IsIdentificationActive())
    {
        // Check if identification time has run out.
        if (remainingIdentificationTimeDs <= passedTimeDs)
        {
            // Make really sure that identification is inactive.
            StopIdentification();
        }
        else
        {
            // Toggle all LEDs under our control on every timer tick as long
            // as identification is active.
            GS->ledRed.Toggle();
            GS->ledGreen.Toggle();
            GS->ledBlue.Toggle();

            //Vibrate until next Timer event
            if(vibrationPins.vibrationPin != -1)
            {
                FruityHal::GpioPinToggle(vibrationPins.vibrationPin);
            }

            //Buzz for a short time
            if(buzzerPins.buzzerPin != -1)
            {
                for(int i=0; i<200; i++)
                {
                    FruityHal::GpioPinToggle(buzzerPins.buzzerPin);
                    FruityHal::DelayUs(500);
                }
                FruityHal::GpioPinClear(buzzerPins.buzzerPin);
            }

            // Adjust the remaining identification time.
            remainingIdentificationTimeDs -= passedTimeDs;
        }
    }
    else
    {
        // If power optimizations are enabled for board we keep LEDs off - only blink for identification
        if (Boardconfig->powerOptimizationEnabled)
        {
            GS->ledRed.Off();
            GS->ledGreen.Off();
            GS->ledBlue.Off();
            return;
        }

        // Identification overrides any other LED activity / mode.

        //If the Beacon is in the enrollment network
        if(currentLedMode == LedMode::CONNECTIONS && GS->node.configuration.networkId == 1){

            GS->ledRed.On();
            GS->ledGreen.Off();
            GS->ledBlue.Off();

        }
        else if (currentLedMode == LedMode::CONNECTIONS)
        {
            //Calculate the current blink step
            ledBlinkPosition = (ledBlinkPosition + 1) % (((GS->config.meshMaxInConnections + Conf::GetInstance().meshMaxOutConnections) + 2) * 2);

            //No Connections: Red blinking, Connected: Green blinking for connection count

            BaseConnections conns = GS->cm.GetBaseConnections(ConnectionDirection::INVALID);
            u8 countHandshakeDone = 0;
            for(u32 i=0; i< conns.count; i++){
                BaseConnection *conn = conns.handles[i].GetConnection();
                if(conn != nullptr && conn->HandshakeDone()) countHandshakeDone++;
            }
            
            u8 i = ledBlinkPosition / 2;
            if(i < (Conf::GetInstance().meshMaxInConnections + Conf::GetInstance().meshMaxOutConnections)){
                if(ledBlinkPosition % 2 == 0){
                    //No connections
                    if (conns.count == 0){ GS->ledRed.On(); }
                    //Connected and handshake done
                    else if(i < countHandshakeDone) { GS->ledGreen.On(); }
                    //Connected and handshake not done
                    else if(i < conns.count) { GS->ledBlue.On(); }
                    //A free connection
                    else if(i < (GS->config.meshMaxInConnections + Conf::GetInstance().meshMaxOutConnections)) {  }
                } else {
                    GS->ledRed.Off();
                    GS->ledGreen.Off();
                    GS->ledBlue.Off();
                }
            }
        }
        else if(currentLedMode == LedMode::ON)
        {
            //All LEDs on (orange when only green and red available)
            GS->ledRed.On();
            GS->ledGreen.On();
            GS->ledBlue.On();
        }
        else if(currentLedMode == LedMode::OFF)
        {
            GS->ledRed.Off();
            GS->ledGreen.Off();
            GS->ledBlue.Off();
        }
        else if(currentLedMode == LedMode::CUSTOM)
        {
            // Controlled by other module
        }
    }

    //DIO_INPUT_LAST_HOLD_TIME_#: Update the timestamp if the button is still pressed
    //for use-cases such as dimming
    for(u32 i=0; i<numDigitalInPinSettings; i++){
        if (digitalInPinSettings[i].readMode == DigitalInReadMode::INTERRUPT)
        {
            u8 state = FruityHal::GpioPinRead(digitalInPinSettings[i].pin) ? 1 : 0;
            u8 activeHigh = digitalInPinSettings[i].activeHigh;
            if((state == activeHigh) && GS->appTimerDs >= digitalInPinSettings[i].lastActiveTimeDs + HOLD_TIME_DURATION_DS) {
                digitalInPinSettings[i].lastHoldTimeDs = GS->appTimerDs;
            }
        }
    }
}

#ifdef TERMINAL_ENABLED
TerminalCommandHandlerReturnType IoModule::TerminalCommandHandler(const char* commandArgs[],u8 commandArgsSize)
{
    //React on commands, return true if handled, false otherwise
    if(commandArgsSize >= 3 && TERMARGS(2, moduleName))
    {
        if (TERMARGS(0, "action"))
        {
            NodeId destinationNode = Utility::TerminalArgumentToNodeId(commandArgs[1]);

            //Example:
            if(TERMARGS(3,"pinset"))
            {
                if (commandArgsSize < 6) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
                //Check how many GPIO ports we want to set
                u8 numExtraParams = (u8) (commandArgsSize - 4);
                u8 numPorts = numExtraParams / 2;
                u8 requestHandle = (numExtraParams % 2 == 0) ? 0 : Utility::StringToU8(commandArgs[commandArgsSize - 1]);

                DYNAMIC_ARRAY(buffer, numPorts*SIZEOF_GPIO_PIN_CONFIG);

                //Encode ports + states into the data
                for(int i=0; i<numPorts; i++){
                    gpioPinConfig* p = (gpioPinConfig*) (buffer + i*SIZEOF_GPIO_PIN_CONFIG);
                    p->pinNumber = (u8)strtoul(commandArgs[(i*2)+4], nullptr, 10);
                    p->direction = 1;
                    p->pull = (u8)FruityHal::GpioPullMode::GPIO_PIN_NOPULL;
                    p->set = TERMARGS((i*2)+5, "high") ? 1 : 0;
                }

                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)IoModuleTriggerActionMessages::SET_PIN_CONFIG,
                    requestHandle,
                    buffer,
                    numPorts*SIZEOF_GPIO_PIN_CONFIG,
                    false
                );
                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            else if (TERMARGS(3,"pinread"))
            {
                if (commandArgsSize < 5) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
                if (commandArgsSize >= 5 + MAX_NUM_GPIO_READ_PINS) return TerminalCommandHandlerReturnType::WRONG_ARGUMENT; // too many arguments
                //Check how many GPIO pins we want to get
                u8 numPins = (u8)(commandArgsSize - 4);

                DYNAMIC_ARRAY(pinNumbers, numPins);

                for (u8 i = 0; i < numPins; ++i) {
                    u8 pinNumber = Utility::StringToU8(commandArgs[4 + i]);
                    pinNumbers[i] = pinNumber;
                }

                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)IoModuleTriggerActionMessages::GET_PIN_LEVEL,
                    0,
                    pinNumbers,
                    numPins,
                    false
                );
                return TerminalCommandHandlerReturnType::SUCCESS;
            }
            //E.g. action 635 io led on
            else if(TERMARGS(3,"led"))
            {
                if (commandArgsSize < 5) return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
                IoModuleSetLedMessage data;

                if(TERMARGS(4, "on")) data.ledMode= LedMode::ON;
                else if(TERMARGS(4, "off")) data.ledMode = LedMode::OFF;
                else if(TERMARGS(4, "custom")) data.ledMode = LedMode::CUSTOM;
                else if(TERMARGS(4, "connections")) data.ledMode = LedMode::CONNECTIONS;
                else return TerminalCommandHandlerReturnType::UNKNOWN;

                u8 requestHandle = commandArgsSize >= 6 ? Utility::StringToU8(commandArgs[5]) : 0;

                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)IoModuleTriggerActionMessages::SET_LED,
                    requestHandle,
                    (u8*)&data,
                    1,
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }

            if (TERMARGS(3, "identify"))
            {
                if (commandArgsSize < 5) 
                {
                    return TerminalCommandHandlerReturnType::NOT_ENOUGH_ARGUMENTS;
                }
                // Define the message object.
                IoModuleSetIdentificationMessage data = {};
                // Fill in the identification mode based on the terminal
                // command arguments.
                if (TERMARGS(4, "on"))
                {
                    data.identificationMode = IdentificationMode::IDENTIFICATION_START;
                }
                else if (TERMARGS(4, "off"))
                {
                    data.identificationMode = IdentificationMode::IDENTIFICATION_STOP;
                }
                else
                {
                    return TerminalCommandHandlerReturnType::UNKNOWN;
                }
                // Parse the request handle if available.
                u8 requestHandle = commandArgsSize >= 6 ? Utility::StringToU8(commandArgs[5]) : 0;
                // Turn identification on by sending a start or stop message. This
                // message is also handled by vendor modules to start any vendor
                // identification mechanism.
                SendModuleActionMessage(
                    MessageType::MODULE_TRIGGER_ACTION,
                    destinationNode,
                    (u8)IoModuleTriggerActionMessages::SET_IDENTIFICATION,
                    requestHandle,
                    (u8*)&data,
                    sizeof(IoModuleSetIdentificationMessage),
                    false
                );

                return TerminalCommandHandlerReturnType::SUCCESS;
            }
        }
    }

    //Must be called to allow the module to get and set the config
    return Module::TerminalCommandHandler(commandArgs, commandArgsSize);
}
#endif

//void IoModule::ParseTerminalInputList(string commandName, vector<string> commandArgs)


void IoModule::MeshMessageReceivedHandler(BaseConnection* connection, BaseConnectionSendData* sendData, ConnPacketHeader const * packetHeader)
{
    //Must call superclass for handling
    Module::MeshMessageReceivedHandler(connection, sendData, packetHeader);

    if(packetHeader->messageType == MessageType::MODULE_TRIGGER_ACTION){
        ConnPacketModule const * packet = (ConnPacketModule const *)packetHeader;
        MessageLength dataFieldLength = sendData->dataLength - SIZEOF_CONN_PACKET_MODULE;

        //Check if our module is meant and we should trigger an action
        if(packet->moduleId == moduleId){
            IoModuleTriggerActionMessages actionType = (IoModuleTriggerActionMessages)packet->actionType;
            if(actionType == IoModuleTriggerActionMessages::SET_PIN_CONFIG){

                configuration.ledMode = LedMode::CUSTOM;
                currentLedMode = LedMode::CUSTOM;

                //Parse the data and set the gpio ports to the requested
                for(int i=0; i<dataFieldLength; i+=SIZEOF_GPIO_PIN_CONFIG)
                {
                    gpioPinConfig const * pinConfig = (gpioPinConfig const *)(packet->data + i);

                    if (pinConfig->direction == 0) FruityHal::GpioConfigureInput(pinConfig->pinNumber, (FruityHal::GpioPullMode)pinConfig->pull);
                    else FruityHal::GpioConfigureOutput(pinConfig->pinNumber);

                    if (pinConfig->set) {
                        FruityHal::GpioPinSet(pinConfig->pinNumber);
                    }
                    else {
                        FruityHal::GpioPinClear(pinConfig->pinNumber);
                    }
                }

                //Confirmation
                SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE,
                    packet->header.sender,
                    (u8)IoModuleActionResponseMessages::SET_PIN_CONFIG_RESULT,
                    packet->requestHandle,
                    nullptr,
                    0,
                    false
                );
            }
            //A message to switch on the LEDs
            else if(actionType == IoModuleTriggerActionMessages::SET_LED){

                IoModuleSetLedMessage const * data = (IoModuleSetLedMessage const *)packet->data;

                currentLedMode = data->ledMode;
                configuration.ledMode = data->ledMode;

                //send confirmation
                SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE,
                    packet->header.sender,
                    (u8)IoModuleActionResponseMessages::SET_LED_RESPONSE,
                    packet->requestHandle,
                    nullptr,
                    0,
                    false
                );
            }
            else if(actionType == IoModuleTriggerActionMessages::SET_IDENTIFICATION){

                const auto * data = (IoModuleSetIdentificationMessage const *)packet->data;

                switch (data->identificationMode)
                {
                    case IdentificationMode::IDENTIFICATION_START:
                        logt("IOMOD", "identification started by SET_IDENTIFICATION message");
                        // Set the remaining identification time, which
                        // activates identification.
                        remainingIdentificationTimeDs = defaultIdentificationTimeDs;
                        // Make sure all leds are in the same state.
                        GS->ledRed.Off();
                        GS->ledGreen.Off();
                        GS->ledBlue.Off();
                        break;

                    case IdentificationMode::IDENTIFICATION_STOP:
                        logt("IOMOD", "identification stopped by SET_IDENTIFICATION message");

                        StopIdentification();

                        break;

                    default:
                        break;
                }

                // Send the action response message.
                SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE,
                    packet->header.sender,
                    (u8)IoModuleActionResponseMessages::SET_IDENTIFICATION_RESPONSE,
                    packet->requestHandle,
                    nullptr,
                    0,
                    false
                );
            }
            else if (actionType == IoModuleTriggerActionMessages::GET_PIN_LEVEL) {
                // + 1 because the receiver reads it until there's a 0xff
                constexpr u8 bufSize = MAX_NUM_GPIO_READ_PINS * SIZEOF_IO_MODULE_GET_PIN_MESSAGE + 1;
                DYNAMIC_ARRAY(buf, bufSize);
                IoModuleGetPinMessage* message = (IoModuleGetPinMessage*) buf;

                u8 i;
                for (i = 0; i < dataFieldLength.GetRaw(); ++i) {
                    u8 pinNumber = packet->data[i];
                    FruityHal::GpioConfigureInput(pinNumber, FruityHal::GpioPullMode::GPIO_PIN_NOPULL);
                    bool pinLevel = FruityHal::GpioPinRead(pinNumber);
                    FruityHal::GpioConfigureOutput(pinNumber); // change it to output so save energy
                    message[i].pinNumber = pinNumber;
                    message[i].pinLevel = (u8)pinLevel;
                }
                buf[i * SIZEOF_IO_MODULE_GET_PIN_MESSAGE] = 0xff;

                SendModuleActionMessage(
                    MessageType::MODULE_ACTION_RESPONSE,
                    packet->header.sender,
                    (u8)IoModuleActionResponseMessages::PIN_LEVEL,
                    packet->requestHandle,
                    buf,
                    bufSize,
                    false
                );
            }
        }
    }

    //Parse Module responses
    if(packetHeader->messageType == MessageType::MODULE_ACTION_RESPONSE){
        ConnPacketModule const * packet = (ConnPacketModule const *)packetHeader;

        //Check if our module is meant and we should trigger an action
        if(packet->moduleId == moduleId)
        {
            IoModuleActionResponseMessages actionType = (IoModuleActionResponseMessages)packet->actionType;
            if(actionType == IoModuleActionResponseMessages::SET_PIN_CONFIG_RESULT)
            {
                logjson_partial("MODULE", "{\"nodeId\":%u,\"type\":\"set_pin_config_result\",\"module\":%u,", packet->header.sender, (u8)ModuleId::IO_MODULE);
                logjson("MODULE",  "\"requestHandle\":%u,\"code\":%u}" SEP, packet->requestHandle, 0);
            }
            else if(actionType == IoModuleActionResponseMessages::SET_LED_RESPONSE)
            {
                logjson_partial("MODULE", "{\"nodeId\":%u,\"type\":\"set_led_result\",\"module\":%u,", packet->header.sender, (u8)ModuleId::IO_MODULE);
                logjson("MODULE",  "\"requestHandle\":%u,\"code\":%u}" SEP, packet->requestHandle, 0);
            }
            else if(actionType == IoModuleActionResponseMessages::SET_IDENTIFICATION_RESPONSE)
            {
                logjson_partial("MODULE", "{\"nodeId\":%u,\"type\":\"identify_response\",\"module\":%u,", packet->header.sender, (u8)ModuleId::IO_MODULE);
                logjson("MODULE",  "\"requestHandle\":%u,\"code\":%u}" SEP, packet->requestHandle, 0);
            }
            else if(actionType == IoModuleActionResponseMessages::PIN_LEVEL)
            {

                logjson_partial("MODULE", "{\"nodeId\":%u,\"type\":\"pin_level_result\",\"module\":%u,\"pins\":[", packet->header.sender, (u8)ModuleId::IO_MODULE);

                const IoModuleGetPinMessage* msgs = (const IoModuleGetPinMessage*)(packet->data);
                for(u8 i = 0; i < MAX_NUM_GPIO_READ_PINS; ++i)
                {
                    u8 pinNumber = msgs[i].pinNumber;
                    if (pinNumber == END_OF_PIN_ARRAY_SYMBOL) break;
                    u8 pinLevel = msgs[i].pinLevel;
                    if(i > 0){ logjson_partial("MODULE", ","); }
                    logjson_partial("MODULE", "{\"pin_number\":%u,\"pin_level\":%u}", pinNumber, pinLevel);
                }
                logjson("MODULE", "],\"requestHandle\":%u,\"code\":%u}" SEP, packet->requestHandle, 0);
            }
        }
    }
}

MeshAccessAuthorization IoModule::CheckMeshAccessPacketAuthorization(BaseConnectionSendData *sendData, u8 const *data,
                                                                     FmKeyId fmKeyId, DataDirection direction)
{
    const auto *packet = (ConnPacketHeader const *)data;

    if (packet->messageType == MessageType::MODULE_TRIGGER_ACTION ||
        packet->messageType == MessageType::MODULE_ACTION_RESPONSE)
    {
        const auto *mod = (ConnPacketModule const *)data;
        if (mod->moduleId == moduleId)
        {
            // MeshAccess connections using the organization key are allowed to change the LED mode and trigger
            // identification.
            if (fmKeyId == FmKeyId::ORGANIZATION)
            {
                if (packet->messageType == MessageType::MODULE_TRIGGER_ACTION)
                {
                    switch (mod->actionType)
                    {
                    case (u8)IoModuleTriggerActionMessages::SET_LED:
                    case (u8)IoModuleTriggerActionMessages::SET_IDENTIFICATION:
                        return MeshAccessAuthorization::WHITELIST;

                    default:
                        return MeshAccessAuthorization::UNDETERMINED;
                    }
                }

                if (packet->messageType == MessageType::MODULE_ACTION_RESPONSE)
                {
                    switch (mod->actionType)
                    {
                    case (u8)IoModuleActionResponseMessages::SET_LED_RESPONSE:
                    case (u8)IoModuleActionResponseMessages::SET_IDENTIFICATION_RESPONSE:
                        return MeshAccessAuthorization::WHITELIST;

                    default:
                        return MeshAccessAuthorization::UNDETERMINED;
                    }
                }
            }
        }
    }

    return MeshAccessAuthorization::UNDETERMINED;
}

bool IoModule::IsIdentificationActive() const
{
    // The remaining time is non-zero if and only if identification is
    // currently active.
    return remainingIdentificationTimeDs > 0;
}

void IoModule::StopIdentification()
{
    //Clear the remaining time
    remainingIdentificationTimeDs = 0;

    //Stop all kinds of notification
    GS->ledRed.Off();
    GS->ledGreen.Off();
    GS->ledBlue.Off();

    if(vibrationPins.vibrationPin >= 0) FruityHal::GpioPinClear(vibrationPins.vibrationPin);
    if(buzzerPins.buzzerPin >= 0) FruityHal::GpioPinClear(buzzerPins.buzzerPin);
}

#if IS_ACTIVE(REGISTER_HANDLER)
RegisterGeneralChecks IoModule::GetGeneralChecks(u16 component, u16 reg, u16 length) const
{
    //We enable the RegisterHandler only for the implemented component id
    if (component == (u16)IoModuleComponent::BASIC_REGISTER_HANDLER_FUNCTIONALITY) return RegisterGeneralChecks::RGC_SUCCESS;

    return RegisterGeneralChecks::RGC_LOCATION_DISABLED;
}

void IoModule::MapRegister(u16 component, u16 register_, SupervisedValue& out, u32& persistedId)
{
    if (component == (u16)IoModuleComponent::BASIC_REGISTER_HANDLER_FUNCTIONALITY)
    {
        //Information Registers
        if(register_ == REGISTER_DIO_OUTPUT_NUM) out.SetReadable(numDigitalOutPinSettings);
        if(register_ == REGISTER_DIO_INPUT_NUM) out.SetReadable(numDigitalInPinSettings);
        if(register_ == REGISTER_DIO_INPUT_TOGGLE_PAIR_NUM) out.SetReadable(numDigitalInTogglePairSettings);

        //DIO_OUTPUT_STATE_#: Make the GPIO output pin states readable and writable
        for(u32 i=0; i<numDigitalOutPinSettings; i++){
            if(register_ == REGISTER_DIO_OUTPUT_STATE_START + i) out.SetWritable(&digitalOutPinSettings[i].state);
        }

        //DIO_INPUT_STATE_#: Make the GPIO input pin states readable
        for(u32 i=0; i<numDigitalInPinSettings; i++){
            if (register_ == REGISTER_DIO_INPUT_STATE_START + i)
            {
                u8 gpioVal = FruityHal::GpioPinRead(digitalInPinSettings[i].pin) ? 1 : 0;
                if (!digitalInPinSettings[i].activeHigh) gpioVal = !gpioVal;
                out.SetReadable(gpioVal);
            }
        }
        
        //DIO_TOGGLE_PAIR_#: Make the last toggled states for the interrupt based inputs readable
        for(u32 i=0; i<numDigitalInTogglePairSettings; i++){
            if (register_ == REGISTER_DIO_TOGGLE_PAIR_START + i)
            {
                u8 indexA = digitalInTogglePairSettings[i].pinIndexA;
                u8 indexB = digitalInTogglePairSettings[i].pinIndexB;
                u8 togglePairState = digitalInPinSettings[indexA].lastActiveTimeDs < digitalInPinSettings[indexB].lastActiveTimeDs;
                out.SetReadable(togglePairState);
            }
        }

        //DIO_INPUT_LAST_ACTIVE_TIME_#: Make the last toggled states for the interrupt based inputs readable
        for(u32 i=0; i<numDigitalInPinSettings; i++){
            if (register_ == REGISTER_DIO_INPUT_LAST_ACTIVE_TIME_START + i * sizeof(u32) && digitalInPinSettings[i].readMode == DigitalInReadMode::INTERRUPT)
            {
                out.SetReadable(digitalInPinSettings[i].lastActiveTimeDs);
            }
        }
        
        //DIO_INPUT_LAST_HOLD_TIME_#: Make the last toggled states for the interrupt based inputs readable
        for(u32 i=0; i<numDigitalInPinSettings; i++){
            if (register_ == REGISTER_DIO_INPUT_LAST_HOLD_TIME_START + i * sizeof(u32) && digitalInPinSettings[i].readMode == DigitalInReadMode::INTERRUPT)
            {
                out.SetReadable(digitalInPinSettings[i].lastHoldTimeDs);
            }
        }
    }
}

void IoModule::ChangeValue(u16 component, u16 register_, u8* values, u16 length)
{
    if (component == 0)
    {
        //DIO_OUTPUT_STATE_#
        //Set the GPIO output pins to the correct state once they are written
        for(u32 i=0; i<numDigitalOutPinSettings; i++){
            if(register_ == REGISTER_DIO_OUTPUT_STATE_START + i){
                //Make sure the register cotains either 0 or 1
                if(values[0] > 1) values[0] = 1;
                u8 value = values[0];

                //Set the Pin according to the received value
                if(digitalOutPinSettings[i].activeHigh){
                    if(value > 0 ) FruityHal::GpioPinSet(digitalOutPinSettings[i].pin);
                        else FruityHal::GpioPinClear(digitalOutPinSettings[i].pin);
                } else {
                    if(value > 0 ) FruityHal::GpioPinClear(digitalOutPinSettings[i].pin);
                        else FruityHal::GpioPinSet(digitalOutPinSettings[i].pin);
                }
            }
        }
    }
}
#endif //IS_ACTIVE(REGISTER_HANDLER)

void IoModule::GpioHandler(u32 pin, FruityHal::GpioTransition transition)
{
    //trace("interrupt pin %u\n", pin);
    //Once a digital-in interrupt triggers, we update the lastActiveTime for this entry
    IoModule* ioMod = (IoModule*)GS->node.GetModuleById(ModuleId::IO_MODULE);
    for (u32 i = 0; i < ioMod->numDigitalInPinSettings; i++) {
        if (ioMod->digitalInPinSettings[i].pin == pin) {
            ioMod->digitalInPinSettings[i].lastActiveTimeDs = GS->appTimerDs;
            break;
        }
    }
}

void IoModule::RequestData(u16 component, u16 register_, u8 length, AutoSenseModuleDataConsumer* provideTo)
{
#if IS_ACTIVE(REGISTER_HANDLER)
    DYNAMIC_ARRAY(buffer, length);
    GetRegisterValues(component, register_, buffer, length);
    provideTo->ConsumeData(ModuleId::IO_MODULE, component, register_, length, buffer);
#endif //IS_ACTIVE(REGISTER_HANDLER)
}