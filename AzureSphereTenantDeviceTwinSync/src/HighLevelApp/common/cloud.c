/* Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License. */


/* Note that this is a modified version of the Azure IoT sample from the Azure Sphere samples github repo */
/* Please see the accompanying README.md for instructions for integrating these code files back into the sample */

#include <memory.h>
#include <stdlib.h>

#include <applibs/eventloop.h>
#include <applibs/log.h>

#include "parson.h"

#include "azure_iot.h"
#include "cloud.h"
#include "exitcodes.h"

// This file implements the interface described in cloud.h in terms of an Azure IoT Hub.
// Specifically, it translates Azure IoT Hub specific concepts (events, device twin messages, device
// methods, etc) into business domain concepts (telemetry, upload enabled, alarm raised)

static const char azureSphereModelId[] = "dtmi:com:example:azuresphere:thermometer;1";

// Azure IoT Hub callback handlers
static void DeviceTwinCallbackHandler(const char *nullTerminatedJsonString);
static int DeviceMethodCallbackHandler(const char *methodName, const unsigned char *payload,
                                       size_t payloadSize, unsigned char **response,
                                       size_t *responseSize);
static void ConnectionChangedCallbackHandler(bool connected);

// Default handlers for cloud events
static void DefaultTelemetryUploadEnabledChangedHandler(bool uploadEnabled);
static void DefaultDisplayAlertHandler(const char *alertMessage);
static void DefaultConnectionChangedHandler(bool connected);

// Cloud event callback handlers
static Cloud_TelemetryUploadEnabledChangedCallbackType
    thermometerTelemetryUploadEnabledChangedCallbackFunction =
        DefaultTelemetryUploadEnabledChangedHandler;
static Cloud_DisplayAlertCallbackType displayAlertCallbackFunction = DefaultDisplayAlertHandler;
static Cloud_ConnectionChangedCallbackType connectionChangedCallbackFunction =
    DefaultConnectionChangedHandler;

// Utility functions
static Cloud_Result AzureIoTToCloudResult(AzureIoT_Result result);

// Constants
#define MAX_PAYLOAD_SIZE 512

unsigned int latestVersion = 1;

bool appRestartEventPending = true;
bool noUpdateAvailableEventPending = false;
bool updateInstallingEventPending = false;

#include <eventloop_timer_utilities.h>

static EventLoopTimer *eventTimer = NULL;


Cloud_Result Cloud_SendEvent(const char *eventName, void *context);


static void EventTimerCallbackHandler(EventLoopTimer *timer)
{
    if (ConsumeEventLoopTimerEvent(timer) != 0) {
        //exitCode = ExitCode_TelemetryTimer_Consume;
        return;
    }

    if(noUpdateAvailableEventPending)
    {
        Log_Debug("Trying to send NoUpdateAvailable event.\n");
        int success = Cloud_SendEvent("NoUpdateAvailable", &noUpdateAvailableEventPending);
        if(success == Cloud_Result_OK)
        {
            // do this on callback
            //noUpdateAvailableEventPending = false;            
        }
        return;
    }
    if(updateInstallingEventPending)
    {
        Log_Debug("Trying to send UpdateInstalling event.\n");
        int success = Cloud_SendEvent("UpdateInstalling", &updateInstallingEventPending);
        if(success == Cloud_Result_OK)
        {
            // do this on callback
            //updateInstallingEventPending = false;
        }
        return;
    }
    if(appRestartEventPending)
    {
        Log_Debug("Trying to send AppRestart event.\n");
        int success = Cloud_SendEvent("AppRestart", &appRestartEventPending);
        if(success == Cloud_Result_OK)
        {
            // do this on callback
            //appRestartEventPending = false;
        }
        return;
    }
}

static void TelemetryCallbackHandler(bool success, void *context)
{
    if(context == &updateInstallingEventPending)
    {
        if(success)
        {
            Log_Debug("UpdateInstalling event sent to IoT Hub OK.\n");
            updateInstallingEventPending = false;
        }
        else
        {
            Log_Debug("UpdateInstalling event send failed to IoT Hub OK.\n");
        }
    }
    
    if(context == &noUpdateAvailableEventPending)
    {
        if(success)
        {
            Log_Debug("NoUpdateAvailable event sent to IoT Hub OK.\n");
            noUpdateAvailableEventPending = false;
        }
        else
        {
            Log_Debug("NoUpdateAvailable event send failed to IoT Hub OK.\n");
        }
    }

    if(context == &appRestartEventPending)
    {
        if(success)
        {
            Log_Debug("AppRestart event sent to IoT Hub OK.\n");
            appRestartEventPending = false;
        }
        else
        {
            Log_Debug("AppRestart event send failed to IoT Hub OK.\n");
        }
    }
}

ExitCode Cloud_Initialize(EventLoop *el, void *backendContext,
                          ExitCode_CallbackType failureCallback,
                          Cloud_TelemetryUploadEnabledChangedCallbackType
                              thermometerTelemetryUploadEnabledChangedCallback,
                          Cloud_DisplayAlertCallbackType displayAlertCallback,
                          Cloud_ConnectionChangedCallbackType connectionChangedCallback)
{
    if (thermometerTelemetryUploadEnabledChangedCallback != NULL) {
        thermometerTelemetryUploadEnabledChangedCallbackFunction =
            thermometerTelemetryUploadEnabledChangedCallback;
    }

    if (displayAlertCallback != NULL) {
        displayAlertCallbackFunction = displayAlertCallback;
    }

    if (connectionChangedCallback != NULL) {
        connectionChangedCallbackFunction = connectionChangedCallback;
    }

    struct timespec eventCheckPeriod = {.tv_sec = 5, .tv_nsec = 0};
    eventTimer =
        CreateEventLoopPeriodicTimer(el, &EventTimerCallbackHandler, &eventCheckPeriod);
    if (eventTimer == NULL) {
        return ExitCode_Init_TelemetryTimer; // need new value
    }

    AzureIoT_Callbacks callbacks = {
        .connectionStatusCallbackFunction = ConnectionChangedCallbackHandler,
        .deviceTwinReceivedCallbackFunction = DeviceTwinCallbackHandler,
        .deviceTwinReportStateAckCallbackTypeFunction = NULL,
        .sendTelemetryCallbackFunction = TelemetryCallbackHandler,
        .deviceMethodCallbackFunction = DeviceMethodCallbackHandler};

    return AzureIoT_Initialize(el, failureCallback, azureSphereModelId, backendContext, callbacks);
}

void Cloud_Cleanup(void)
{
    DisposeEventLoopTimer(eventTimer);
    AzureIoT_Cleanup();
}

static Cloud_Result AzureIoTToCloudResult(AzureIoT_Result result) {
    switch (result) {
    case AzureIoT_Result_OK:
        return Cloud_Result_OK;
    case AzureIoT_Result_NoNetwork:
        return Cloud_Result_NoNetwork;
    case AzureIoT_Result_OtherFailure:
    default:
        return Cloud_Result_OtherFailure;
    }
}

Cloud_Result Cloud_SendTelemetry(const Cloud_Telemetry *telemetry)
{
    JSON_Value *telemetryValue = json_value_init_object();
    JSON_Object *telemetryRoot = json_value_get_object(telemetryValue);
    json_object_dotset_number(telemetryRoot, "temperature", telemetry->temperature);
    char *serializedTelemetry = json_serialize_to_string(telemetryValue);
    AzureIoT_Result aziotResult = AzureIoT_SendTelemetry(serializedTelemetry, NULL);
    Cloud_Result result = AzureIoTToCloudResult(aziotResult);

    json_free_serialized_string(serializedTelemetry);
    json_value_free(telemetryValue);

    return result;
}

void Cloud_SignalNoUpdatePending(void) 
{
    noUpdateAvailableEventPending = true;
}

void Cloud_SignalUpdateInstalling(void)
{
    updateInstallingEventPending = true;
}

Cloud_Result Cloud_SendEvent(const char *eventName, void *context)
{
    JSON_Value *eventValue = json_value_init_object();
    JSON_Object *eventRoot = json_value_get_object(eventValue);
    json_object_dotset_boolean(eventRoot, eventName, 1);
    char *serializedEvent = json_serialize_to_string(eventValue);
    AzureIoT_Result aziotResult = AzureIoT_SendTelemetry(serializedEvent, context);
    Cloud_Result result = AzureIoTToCloudResult(aziotResult);

    json_free_serialized_string(serializedEvent);
    json_value_free(eventValue);

    return result;
}

Cloud_Result Cloud_SendThermometerMovedEvent(void)
{
    JSON_Value *thermometerMovedValue = json_value_init_object();
    JSON_Object *thermometerMovedRoot = json_value_get_object(thermometerMovedValue);
    json_object_dotset_boolean(thermometerMovedRoot, "thermometerMoved", 1);
    char *serializedDeviceMoved = json_serialize_to_string(thermometerMovedValue);
    AzureIoT_Result aziotResult = AzureIoT_SendTelemetry(serializedDeviceMoved, NULL);
    Cloud_Result result = AzureIoTToCloudResult(aziotResult);

    json_free_serialized_string(serializedDeviceMoved);
    json_value_free(thermometerMovedValue);

    return result;
}

Cloud_Result Cloud_SendThermometerTelemetryUploadEnabledChangedEvent(bool uploadEnabled)
{
    JSON_Value *thermometerTelemetryUploadValue = json_value_init_object();
    JSON_Object *thermometerTelemetryUploadRoot =
        json_value_get_object(thermometerTelemetryUploadValue);

    json_object_dotset_boolean(thermometerTelemetryUploadRoot,
                               "thermometerTelemetryUploadEnabled.value", uploadEnabled ? 1 : 0);
    json_object_dotset_number(thermometerTelemetryUploadRoot,
                              "thermometerTelemetryUploadEnabled.ac", 200);
    json_object_dotset_number(thermometerTelemetryUploadRoot,
                              "thermometerTelemetryUploadEnabled.av", latestVersion++);
    json_object_dotset_string(thermometerTelemetryUploadRoot,
                              "thermometerTelemetryUploadEnabled.ad",
                              "Successfully updated thermometerTelemetryUploadEnabled");
    char *serializedTelemetryUpload = json_serialize_to_string(thermometerTelemetryUploadValue);
    AzureIoT_Result aziotResult = AzureIoT_DeviceTwinReportState(serializedTelemetryUpload, NULL);
    Cloud_Result result = AzureIoTToCloudResult(aziotResult);

    json_free_serialized_string(serializedTelemetryUpload);
    json_value_free(thermometerTelemetryUploadValue);

    return result;
}

Cloud_Result Cloud_SendDeviceDetails(const char *serialNumber)
{
    // Send static device twin properties when connection is established.
    JSON_Value *deviceDetailsValue = json_value_init_object();
    JSON_Object *deviceDetailsRoot = json_value_get_object(deviceDetailsValue);
    json_object_dotset_string(deviceDetailsRoot, "serialNumber", serialNumber);
    char *serializedDeviceDetails = json_serialize_to_string(deviceDetailsValue);
    AzureIoT_Result aziotResult = AzureIoT_DeviceTwinReportState(serializedDeviceDetails, NULL);
    Cloud_Result result = AzureIoTToCloudResult(aziotResult);

    json_free_serialized_string(serializedDeviceDetails);
    json_value_free(deviceDetailsValue);

    return result;
}

static void DefaultTelemetryUploadEnabledChangedHandler(bool uploadEnabled)
{
    Log_Debug("WARNING: Cloud - no handler registered for TelemetryUploadEnabled - status %s\n",
              uploadEnabled ? "true" : "false");
}

static void DefaultDisplayAlertHandler(const char *alertMessage)
{
    Log_Debug("WARNING: Cloud - no handler registered for DisplayAlert - message %s\n",
              alertMessage);
}

static void DefaultConnectionChangedHandler(bool connected)
{
    Log_Debug("WARNING: Cloud - no handler registered for ConnectionChanged - status %s\n",
              connected ? "true" : "false");
}

static void ConnectionChangedCallbackHandler(bool connected)
{
    connectionChangedCallbackFunction(connected);
}

static void DeviceTwinCallbackHandler(const char *nullTerminatedJsonString)
{
    JSON_Value *rootProperties = NULL;
    rootProperties = json_parse_string(nullTerminatedJsonString);
    if (rootProperties == NULL) {
        Log_Debug("WARNING: Cannot parse the string as JSON content.\n");
        goto cleanup;
    }

    JSON_Object *rootObject = json_value_get_object(rootProperties);
    JSON_Object *desiredProperties = json_object_dotget_object(rootObject, "desired");
    if (desiredProperties == NULL) {
        desiredProperties = rootObject;
    }

    // The desired properties should have a "TelemetryUploadEnabled" object
    int thermometerTelemetryUploadEnabledValue =
        json_object_dotget_boolean(desiredProperties, "thermometerTelemetryUploadEnabled");

    if (thermometerTelemetryUploadEnabledValue != -1) {
        unsigned int requestedVersion =
            (unsigned int)json_object_dotget_number(desiredProperties, "$version");

        if (requestedVersion > latestVersion) {
            latestVersion = requestedVersion;
        }

        thermometerTelemetryUploadEnabledChangedCallbackFunction(
            thermometerTelemetryUploadEnabledValue == 1);
    }

cleanup:
    // Release the allocated memory.
    json_value_free(rootProperties);
}

static int DeviceMethodCallbackHandler(const char *methodName, const unsigned char *payload,
                                       size_t payloadSize, unsigned char **response,
                                       size_t *responseSize)
{
    int result;
    char *responseString;
    static char nullTerminatedPayload[MAX_PAYLOAD_SIZE + 1];

    size_t actualPayloadSize = payloadSize > MAX_PAYLOAD_SIZE ? MAX_PAYLOAD_SIZE : payloadSize;

    strncpy(nullTerminatedPayload, payload, actualPayloadSize);
    nullTerminatedPayload[actualPayloadSize] = '\0';

    if (strcmp("displayAlert", methodName) == 0) {

        displayAlertCallbackFunction(nullTerminatedPayload);

        responseString =
            "\"Alert message displayed successfully.\""; // must be a JSON string (in quotes)
        result = 200;
    } else {
        // All other method names are ignored
        responseString = "{}";
        result = -1;
    }

    // if 'response' is non-NULL, the Azure IoT library frees it after use, so copy it to heap
    *responseSize = strlen(responseString);
    *response = malloc(*responseSize);
    memcpy(*response, responseString, *responseSize);

    return result;
}
