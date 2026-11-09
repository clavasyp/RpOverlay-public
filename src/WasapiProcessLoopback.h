#pragma once

// ---------------------------------------------------------------------------
//  WasapiProcessLoopback.h
//
//  Ручные объявления типов и констант WASAPI process-loopback (Windows 10
//  2004+ / "ActivateAudioInterfaceAsync на VAD\\Process_Loopback").
//
//  Зачем: в MinGW (ucrt64) заголовки <audioclientactivationparams.h> и
//  соответствующие определения PROCESS_LOOPBACK_* отсутствуют, хотя сам
//  ActivateAudioInterfaceAsync и интерфейсы IAudioClient / IMMNotificationClient
//  объявлены в mmdeviceapi.h / audioclient.h. Поэтому здесь доопределяем ТОЛЬКО
//  недостающие активационные структуры — интерфейсы НЕ переобъявляем.
//
//  Подключать ПОСЛЕ <windows.h>, <mmdeviceapi.h>, <audioclient.h>.
// ---------------------------------------------------------------------------

#include <windows.h>

// Виртуальное устройство для process-loopback активации.
#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#  define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

// AUTOCONVERTPCM / SRC_DEFAULT_QUALITY — могут отсутствовать в старых заголовках.
#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#  define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#  define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

// ---------------------------------------------------------------------------
//  Активационные структуры (обычно из audioclientactivationparams.h).
//  Под защитой include-guard'ов на случай, если в будущей версии MinGW они
//  всё-таки появятся.
// ---------------------------------------------------------------------------
#ifndef __audioclientactivationparams_h__
#define __audioclientactivationparams_h__

typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD                 TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT          = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

#endif // __audioclientactivationparams_h__
