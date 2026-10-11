// efi.h - the parts of the UEFI specification the loader uses
//
// Only what Terminal OS's loader calls: the system table, boot and runtime
// services, text in and out, files, graphics, block devices and device paths.
// Layouts follow the UEFI 2.x specification; every call uses the Microsoft
// x64 calling convention (EFIAPI).
#ifndef EFI_H
#define EFI_H

#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_SUCCESS               0
#define EFI_ERR                   (1ull << 63)
#define EFI_LOAD_ERROR            (EFI_ERR | 1)
#define EFI_INVALID_PARAMETER     (EFI_ERR | 2)
#define EFI_UNSUPPORTED           (EFI_ERR | 3)
#define EFI_BUFFER_TOO_SMALL      (EFI_ERR | 5)
#define EFI_NOT_READY             (EFI_ERR | 6)
#define EFI_NOT_FOUND             (EFI_ERR | 14)
#define EFI_ERROR(s)              (((s) & EFI_ERR) != 0)

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---- Text ---------------------------------------------------------------- */

typedef struct { uint16_t ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, const CHAR16 *);
    void *TestString, *QueryMode, *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, uint64_t);
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *SetCursorPosition, *EnableCursor, *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Memory ---------------------------------------------------------------- */

typedef enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory, EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress } EFI_ALLOCATE_TYPE;

typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_MEMORY_RUNTIME (1ull << 63)

/* ---- Services ---------------------------------------------------------------- */

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE, uint64_t, EFI_PHYSICAL_ADDRESS *);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, uint64_t);
    EFI_STATUS (EFIAPI *GetMemoryMap)(uint64_t *, EFI_MEMORY_DESCRIPTOR *, uint64_t *, uint64_t *, uint32_t *);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, uint64_t, void **);
    EFI_STATUS (EFIAPI *FreePool)(void *);
    void *CreateEvent, *SetTimer;
    EFI_STATUS (EFIAPI *WaitForEvent)(uint64_t, EFI_EVENT *, uint64_t *);
    void *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, uint64_t);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(uint64_t);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(uint64_t, uint64_t, uint64_t, const CHAR16 *);
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *, void *, void **);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    void *GetVariable, *GetNextVariableName, *SetVariable;
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(uint32_t, EFI_STATUS, uint64_t, void *);
} EFI_RUNTIME_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    uint64_t NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Images, devices, files ------------------------------------------------------ */

typedef struct { uint8_t Type, SubType, Length[2]; } EFI_DEVICE_PATH;

#define DP_MEDIA          4
#define DP_MEDIA_HD       1
#define DP_MEDIA_FILE     4
#define DP_MESSAGING      3
#define DP_MSG_USB        5
#define DP_END            0x7F

typedef struct {
    EFI_DEVICE_PATH Header;
    uint32_t PartitionNumber;
    uint64_t PartitionStart, PartitionSize;
    uint8_t Signature[16];
    uint8_t MBRType, SignatureType;
} __attribute__((packed)) EFI_HD_DEVICE_PATH;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    EFI_MEMORY_TYPE ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, const CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *, uint64_t *, void *);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(struct EFI_FILE_PROTOCOL *, uint64_t *);
    EFI_STATUS (EFIAPI *SetPosition)(struct EFI_FILE_PROTOCOL *, uint64_t);
    void *GetInfo, *SetInfo, *Flush;
} EFI_FILE_PROTOCOL;

#define EFI_FILE_MODE_READ 1

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    uint32_t MediaId;
    BOOLEAN RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
    uint32_t BlockSize, IoAlign;
    uint64_t LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA *Media;
} EFI_BLOCK_IO_PROTOCOL;

/* ---- Graphics ------------------------------------------------------------------- */

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor, PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask, PixelBltOnly
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    uint64_t SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    uint64_t FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void *QueryMode, *SetMode, *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- GUIDs ---------------------------------------------------------------------- */

#define LOADED_IMAGE_GUID   { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define SIMPLE_FS_GUID      { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define DEVICE_PATH_GUID    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define BLOCK_IO_GUID       { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define GOP_GUID            { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define ACPI20_GUID         { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }
#define ACPI10_GUID         { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }

#endif
