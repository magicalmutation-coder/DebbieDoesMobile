#pragma once
/*
 * memory_manager.h — Debbie's short-term & long-term memory
 *
 * Short-term memory: a circular buffer of the last MEMORY_MAX_TURNS
 *   conversation turns (user + assistant) kept in RAM.
 *
 * Long-term memory: up to MEMORY_MAX_FACTS key-value "facts" (e.g.
 *   "user_name=Alice", "user_likes=jazz") persisted to NVS flash.
 *
 * RAG: on request, queries the companion server's /memory/query endpoint
 *   and appends the retrieved context to the system prompt.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ── Limits ───────────────────────────────────────────────────────────────── */
#define MEMORY_MAX_TURNS    20      /* recent turns kept in RAM */
#define MEMORY_MAX_FACTS    50      /* long-term facts in NVS */
#define MEMORY_FACT_KEY_MAX 48      /* max chars for a fact key */
#define MEMORY_FACT_VAL_MAX 192     /* max chars for a fact value */
#define MEMORY_TURN_TEXT_MAX 512    /* max chars per turn */
#define MEMORY_CONTEXT_MAX  4096    /* max chars in built context blob */

/* ── Types ────────────────────────────────────────────────────────────────── */

typedef struct {
    char    role[12];                       /* "user" or "assistant" */
    char    text[MEMORY_TURN_TEXT_MAX];
    int64_t timestamp_ms;
} memory_turn_t;

typedef struct {
    char    key[MEMORY_FACT_KEY_MAX];
    char    value[MEMORY_FACT_VAL_MAX];
    int64_t timestamp_ms;
    uint8_t importance;                     /* 0-10 */
} memory_fact_t;

/* ── Lifecycle ────────────────────────────────────────────────────────────── */

/**
 * @brief  Initialise memory manager.
 *         Loads saved facts (and last-N turns) from NVS.
 *         Call once after storage_init().
 */
esp_err_t memory_manager_init(void);

/**
 * @brief  Persist current facts (and last 5 turns) to NVS.
 *         Called automatically after each turn; call explicitly on shutdown.
 */
esp_err_t memory_manager_save(void);

/**
 * @brief  Erase all in-RAM and NVS memory.
 */
esp_err_t memory_manager_clear(void);

/* ── Short-term memory ────────────────────────────────────────────────────── */

/**
 * @brief  Append a conversation turn to the in-RAM circular buffer.
 *
 * @param role  "user" or "assistant"
 * @param text  Transcribed text
 */
void memory_manager_add_turn(const char *role, const char *text);

/**
 * @brief  Return a pointer to the turn circular buffer (read-only).
 *         Use memory_manager_turn_count() to know how many are valid.
 */
const memory_turn_t *memory_manager_get_turns(void);

/** @brief  Number of turns currently in the buffer (0 – MEMORY_MAX_TURNS). */
int memory_manager_turn_count(void);

/* ── Long-term facts ──────────────────────────────────────────────────────── */

/**
 * @brief  Store / update a long-term fact in NVS.
 *
 * @param key         Identifier (e.g. "user_name", "user_location")
 * @param value       Value string
 * @param importance  0-10 (higher = kept longer / shown first)
 */
esp_err_t memory_manager_save_fact(const char *key,
                                   const char *value,
                                   uint8_t     importance);

/**
 * @brief  Return all stored facts and their count.
 */
const memory_fact_t *memory_manager_get_facts(int *count_out);

/* ── Context building (for system-prompt injection) ───────────────────────── */

/**
 * @brief  Build a memory context string to prepend to the system prompt.
 *
 *  Format:
 *    [Memory] Facts: user_name=Alice; user_location=London
 *    [Memory] Recent conversation:
 *      user: what's the weather like?
 *      assistant: It's sunny in London today.
 *
 * @return  Heap-allocated string — caller must free().
 *          Returns NULL on allocation failure.
 */
char *memory_manager_build_context(void);

/**
 * @brief  Build an enriched system prompt: base_prompt + memory context.
 *         Returns a heap-allocated string — caller must free().
 */
char *memory_manager_enrich_prompt(const char *base_prompt);

/* ── Companion RAG ────────────────────────────────────────────────────────── */

/**
 * @brief  Query the companion server for memories relevant to `query`.
 *         Returns NULL if companion is not reachable or not configured.
 *         Caller must free() the returned string (it contains a brief
 *         plain-text summary of retrieved memories).
 *
 * @param query  The user's latest utterance.
 */
char *memory_manager_query_rag(const char *query);

/**
 * @brief  Sync a new turn to the companion server's persistent store.
 *         Non-blocking — fires-and-forgets using a short HTTP POST.
 *
 * @param role  "user" or "assistant"
 * @param text  Turn text
 */
void memory_manager_sync_turn(const char *role, const char *text);
