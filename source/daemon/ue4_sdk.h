/*
 * ue4_sdk.h — External UE4 SDK generator.
 *
 * Top-level entry point: given a PID, acquire the Mach task port,
 * resolve ASLR, walk UE4 reflection structures, and write the
 * schema JSON.  Entirely read-only — never writes to the game.
 */

#ifndef UE4_SDK_H
#define UE4_SDK_H

#include <sys/types.h>

/* Default output path for the generated schema. */
#define UE4_SDK_OUTPUT_PATH   "/var/mobile/Downloads/ue4_schema.json"
#define UE4_SDK_LOG_PATH      "/var/mobile/Downloads/ue4_sdk.log"
#define UE4_SDK_CONFIG_PATH   "/var/mobile/Downloads/ue4_sdk_config.txt"

/* Generate the SDK for the process with the given PID.
 * Returns 0 on success, -1 on failure.
 * On failure, diagnostic information is written to UE4_SDK_LOG_PATH. */
int ue4_sdk_generate(pid_t target_pid);

#endif /* UE4_SDK_H */
