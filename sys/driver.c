/*++
    Code for main entry point of KMDF driver
--*/

#include "hidusbsteelbattalion.h"

#if defined(EVENT_TRACING)
//
// The trace message header (.tmh) file must be included in a source file
// before any WPP macro calls and after defining a WPP_CONTROL_GUIDS
// macro (defined in toaster.h). During the compilation, WPP scans the source
// files for DoTraceMessage() calls and builds a .tmh file which stores a unique
// data GUID for each message, the text resource string for each message,
// and the data types of the variables passed in for each message.  This file
// is automatically generated and used during post-processing.
//
#include "driver.tmh"
#else
ULONG DebugLevel = TRACE_LEVEL_INFORMATION;
ULONG DebugFlag = 0xff;
#endif

#ifdef ALLOC_PRAGMA
    #pragma alloc_text( INIT, DriverEntry )
    #pragma alloc_text( PAGE, HidSteelBattalionEvtDeviceAdd)
    #pragma alloc_text( PAGE, HidSteelBattalionEvtDriverContextCleanup)
#endif

NTSTATUS DriverEntry 
(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
/*++
    Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

Arguments:
    DriverObject - pointer to the driver object
    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:
    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.
--*/
{
    NTSTATUS               status = STATUS_SUCCESS;
    WDF_DRIVER_CONFIG      config;
    WDF_OBJECT_ATTRIBUTES  attributes;

    // Initialize WPP Tracing
    WPP_INIT_TRACING( DriverObject, RegistryPath );

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Steel Battalion Controller Driver\n");

    WDF_DRIVER_CONFIG_INIT(&config, HidSteelBattalionEvtDeviceAdd);

    // Register a cleanup callback so that we can call WPP_CLEANUP when
    // the framework driver object is deleted during driver unload.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = HidSteelBattalionEvtDriverContextCleanup;

    // Create a framework driver object to represent our driver.
    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_INIT, "WdfDriverCreate failed with status 0x%x\n", status);
        WPP_CLEANUP(DriverObject);
    }
    return status;
}


NTSTATUS HidSteelBattalionEvtDeviceAdd
(
    IN WDFDRIVER Driver,
    IN PWDFDEVICE_INIT DeviceInit
)
/*++
Routine Description:

    HidSteelBattalionEvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. We create and initialize a WDF device object to
    represent a new instance of toaster device.

Arguments:
    Driver - Handle to a framework driver object created in DriverEntry
    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:
    NTSTATUS
--*/
{
    NTSTATUS                      status = STATUS_SUCCESS;
    WDF_IO_QUEUE_CONFIG           queueConfig;
    WDF_OBJECT_ATTRIBUTES         attributes;
    WDFDEVICE                     hDevice;
    PDEVICE_EXTENSION             devContext = NULL;
    WDFQUEUE                      queue;
    WDF_PNPPOWER_EVENT_CALLBACKS  pnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    PAGED_CODE();

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_PNP, "HidSteelBattalionEvtDeviceAdd called\n");

    // Tell framework this is a filter driver. Filter drivers by default are
    // not power policy owners. This works well for this driver because
    // HIDclass driver is the power policy owner for HID minidrivers.
    WdfFdoInitSetFilter(DeviceInit);

    // Initialize pnp-power callbacks, attributes and a context area for the device object.
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    // For usb devices, PrepareHardware callback is the to place select the
    // interface and configure the device.
    pnpPowerCallbacks.EvtDevicePrepareHardware = HidSteelBattalionEvtDevicePrepareHardware;

    // These two callbacks start and stop the wdfusb pipe continuous reader
    // as we go in and out of the D0-working state.
    pnpPowerCallbacks.EvtDeviceD0Entry = HidSteelBattalionEvtDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit  = HidSteelBattalionEvtDeviceD0Exit;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_EXTENSION);

    // Create a framework device object.This call will in turn create
    // a WDM device object, attach to the lower stack, and set the
    // appropriate flags and attributes.s
    status = WdfDeviceCreate(&DeviceInit, &attributes, &hDevice);
    if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfDeviceCreate failed with status code 0x%x\n", status);
        return status;
    }

    devContext = GetDeviceContext(hDevice);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = HidSteelBattleEvtInternalDeviceControl;

    status = WdfIoQueueCreate(hDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS (status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfIoQueueCreate failed 0x%x\n", status);
        return status;
    }

    // Register a manual I/O queue for handling Interrupt Message Read Requests.
    // This queue will be used for storing Requests that need to wait for an
    // interrupt to occur before they can be completed.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

    // This queue is used for requests that dont directly access the device. The
    // requests in this queue are serviced only when the device is in a fully
    // powered state and sends an interrupt. So we can use a non-power managed
    // queue to park the requests since we dont care whether the device is idle
    // or fully powered up.
    queueConfig.PowerManaged = WdfFalse;

    status = WdfIoQueueCreate(hDevice, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &devContext->InterruptMsgQueue);
    if (!NT_SUCCESS(status)) 
	{
        TraceEvents(TRACE_LEVEL_ERROR, DBG_PNP, "WdfIoQueueCreate failed 0x%x\n", status);
        return status;
    }
    return status;
}


VOID HidSteelBattalionEvtDriverContextCleanup(IN WDFOBJECT Object)
/*++
    Free resources allocated in DriverEntry that are not automatically
    cleaned up framework.

Arguments:
    Driver - handle to a WDF Driver object.

Return Value:
    VOID.
--*/
{
    PAGED_CODE ();
    UNREFERENCED_PARAMETER(Object);

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Exit HidSteelBattalionEvtDriverContextCleanup\n");

    WPP_CLEANUP(WdfDriverWdmGetDriverObject((WDFDRIVER) Object));
}


#if !defined(EVENT_TRACING)

VOID TraceEvents
(
    IN ULONG   TraceEventsLevel,
    IN ULONG   TraceEventsFlag,
    IN PCCHAR  DebugMessage,
    ...
)
/*++
Debug print for the sample driver.

Arguments:
    TraceEventsLevel - print level between 0 and 3, with 3 the most verbose

Return Value:
    None.
 --*/
{
#if DBG
#define     TEMP_BUFFER_SIZE        512
    va_list    list;
    CHAR       debugMessageBuffer[TEMP_BUFFER_SIZE];
    NTSTATUS   status;

    va_start(list, DebugMessage);

    if (DebugMessage) 
	{
        //
        // Using new safe string functions instead of _vsnprintf.
        // This function takes care of NULL terminating if the message
        // is longer than the buffer.
        //
        status = RtlStringCbVPrintfA( debugMessageBuffer,
                                      sizeof(debugMessageBuffer),
                                      DebugMessage,
                                      list );
        if(!NT_SUCCESS(status)) 
		{
            DbgPrint (_DRIVER_NAME_": RtlStringCbVPrintfA failed 0x%x\n", status);
            return;
        }
        if ((TraceEventsLevel <= TRACE_LEVEL_ERROR) ||
            (TraceEventsLevel <= DebugLevel && ((TraceEventsFlag & DebugFlag) == TraceEventsFlag))) 
		{
            DbgPrint("%s%s", _DRIVER_NAME_, debugMessageBuffer);
        }
    }
    va_end(list);

    return;
#else
    UNREFERENCED_PARAMETER(TraceEventsLevel);
    UNREFERENCED_PARAMETER(TraceEventsFlag);
    UNREFERENCED_PARAMETER(DebugMessage);
#endif
}

#endif

