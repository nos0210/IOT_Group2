# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "ADC_DAC-unsigned.bin"
  "ADC_DAC.bin"
  "ADC_DAC.map"
  "AmazonRootCA1.pem.S"
  "bootloader\\bootloader-reflash-digest.bin"
  "bootloader\\bootloader.bin"
  "bootloader\\bootloader.elf"
  "bootloader\\bootloader.map"
  "bootloader\\secure-bootloader-key-192.bin"
  "bootloader\\secure-bootloader-key-256.bin"
  "config\\sdkconfig.cmake"
  "config\\sdkconfig.h"
  "device.crt.S"
  "device.key.S"
  "esp-idf\\mbedtls\\x509_crt_bundle"
  "flash_app_args"
  "flash_bootloader_args"
  "flash_project_args"
  "flasher_args.json"
  "flasher_args.json.in"
  "ldgen_libraries"
  "ldgen_libraries.in"
  "project_elf_src_esp32.c"
  "signature_verification_key.bin"
  "signature_verification_key.bin.S"
  "x509_crt_bundle.S"
  )
endif()
