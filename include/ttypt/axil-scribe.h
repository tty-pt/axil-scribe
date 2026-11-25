#ifndef TTYPT_AXIL_SCRIBE_H
#define TTYPT_AXIL_SCRIBE_H

/**
 * @file axil-scribe.h
 * @brief Axil HTTP service module for axil-scribe.
 *
 * Mounts SSR + JSON endpoints for archival document scan ingestion,
 * split-screen HITL verification, liblemma search, and open exports.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Programmatically initialize the axil-scribe module state.
 *
 * NULL arguments fall back to SCRIBE_* environment variables and local defaults.
 * @return 0 on success, -1 on error.
 */
int axil_scribe_init(const char *data_dir, const char *templates_dir,
                     const char *dict_path);

/**
 * @brief Flush persistence and release module resources.
 */
void axil_scribe_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* TTYPT_AXIL_SCRIBE_H */
