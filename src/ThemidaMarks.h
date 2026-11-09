#pragma once

// Themida-ready маркеры защиты (Слой A плана усиления защиты).
//
// ПО УМОЛЧАНИЮ ВСЕ МАКРОСЫ — NO-OP: обычная сборка/релиз не меняются ни на байт, нулевой оверхед.
// Маркеры стоят в исходнике ВСЕГДА (готовность к защите), но РЕАЛЬНО активируются только флагом
// сборки -DWITH_THEMIDA_SDK=ON, после чего бинарь прогоняется через Themida (scripts/protect-dist.sh) —
// она находит маркеры и виртуализирует/шифрует помеченные участки.
//
// Почему no-op по умолчанию (а не «всегда вкл»):
//   1) Реальная защита материализуется ТОЛЬКО когда Themida обрабатывает exe (GUI/CLI-шаг). Без него
//      маркеры = инертный jmp-over-data, пользы нет, а риск (AV-FP, поломка бесшовного file-swap
//      апдейтера, см. план §A4/§7) есть. Поэтому защищённый билд собирается осознанно и тестируется.
//   2) Тулчейн — MinGW/GCC: VM-маркеры под -O2 могут переставиться → помеченные функции должны
//      собираться с RP_NOOPT (= NO_OPTIMIZATION = __attribute__((optimize("O0")))).
//
// Рекомендация по VM: на этом GCC-тулчейне надёжнее выбирать функции для виртуализации в GUI Themida
// (Functions/Map) — список целей в docs/THEMIDA_PROTECTION_SPEC.md. Инлайн-маркеры здесь — для строк
// (STR_ENCRYPT) и сторожей (CHECK_*), где GUI-выбор не подходит.

#if defined(WITH_THEMIDA_SDK)
  #include "ThemidaSDK.h"                       // из Themida/ThemidaSDK/Include/C (CMake кладёт в include path)
  #define RP_NOOPT                  NO_OPTIMIZATION
  #define RP_VM_BEGIN               VM_START
  #define RP_VM_END                 VM_END
  #define RP_STRENC_BEGIN           STR_ENCRYPT_START
  #define RP_STRENC_END             STR_ENCRYPT_END
  #define RP_STRENCW_BEGIN          STR_ENCRYPTW_START
  #define RP_STRENCW_END            STR_ENCRYPTW_END
  #define RP_MUTATE_BEGIN           MUTATE_START
  #define RP_MUTATE_END             MUTATE_END
  #define RP_CHECK_PROTECTION(v,x)      CHECK_PROTECTION(v, x)
  #define RP_CHECK_CODE_INTEGRITY(v,x)  CHECK_CODE_INTEGRITY(v, x)
  #define RP_CHECK_DEBUGGER(v,x)        CHECK_DEBUGGER(v, x)
#else
  // No-op сборка (по умолчанию). Бинарь идентичен сборке без этого заголовка.
  #define RP_NOOPT
  #define RP_VM_BEGIN
  #define RP_VM_END
  #define RP_STRENC_BEGIN
  #define RP_STRENC_END
  #define RP_STRENCW_BEGIN
  #define RP_STRENCW_END
  #define RP_MUTATE_BEGIN
  #define RP_MUTATE_END
  // Сторож «проходит» в незащищённой сборке: var := val (нет Themida — нечего проверять, тревоги нет).
  #define RP_CHECK_PROTECTION(v,x)      do { (v) = (x); } while (0)
  #define RP_CHECK_CODE_INTEGRITY(v,x)  do { (v) = (x); } while (0)
  #define RP_CHECK_DEBUGGER(v,x)        do { (v) = (x); } while (0)
#endif
