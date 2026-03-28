#ifndef BUILTINS_H
#define BUILTINS_H

#include "vm.h"

void builtin_print(VM* vm);
void builtin_println(VM* vm);
void builtin_len(VM* vm);
void builtin_array_with_size(VM* vm);
void builtin_typeOf(VM* vm);
void builtin_future_pending(VM* vm);
void builtin_future_resolved(VM* vm);
void builtin_future_is_ready(VM* vm);
void builtin_future_complete(VM* vm);
void builtin_future_get(VM* vm);
void builtin_ext_posted_callback_pending_count(VM* vm);
void builtin_ext_drain_posted_callbacks(VM* vm);
void builtin_ext_set_posted_callback_auto_drain(VM* vm);
void builtin_async_sleep(VM* vm);
void builtin_async_channel_send(VM* vm);
void builtin_async_channel_send_typed(VM* vm);
void builtin_async_channel_recv(VM* vm);
void builtin_async_channel_recv_typed(VM* vm);
void builtin_toInt(VM* vm);
void builtin_toDouble(VM* vm);
void builtin_toBigInt(VM* vm);
void builtin_toHexBigInt(VM* vm);
void builtin_bytesToHex(VM* vm);
void builtin_hexToBytes(VM* vm);
void builtin_string_to_bytes(VM* vm);
void builtin_bytes_to_string(VM* vm);
void builtin_sha256_bytes(VM* vm);
void builtin_hmac_sha256_bytes(VM* vm);
void builtin_pbkdf2_hmac_sha256_bytes(VM* vm);
void builtin_hkdf_hmac_sha256_bytes(VM* vm);
void builtin_constant_time_bytes_equal(VM* vm);
void builtin_aes_ctr_bytes(VM* vm);
void builtin_aes_gcm_seal_bytes(VM* vm);
void builtin_aes_gcm_open_bytes(VM* vm);
void builtin_bytes_join(VM* vm);
void builtin_url_encode(VM* vm);
void builtin_url_decode(VM* vm);
void builtin_str(VM* vm);
void builtin_format_double(VM* vm);
void builtin_json_parse(VM* vm);
void builtin_json_stringify(VM* vm);
void builtin_json_stringify_pretty(VM* vm);
void builtin_json_decode(VM* vm);
void builtin_push(VM* vm);
void builtin_pop(VM* vm);
void builtin_copy_into(VM* vm);
void builtin_reverse_prefix(VM* vm);
void builtin_rotate_prefix_left(VM* vm);
void builtin_rotate_prefix_right(VM* vm);
void builtin_keys(VM* vm);
void builtin_values(VM* vm);

void register_builtins(VM* vm);

// File I/O functions
void builtin_read_line(VM* vm);
void builtin_read_all(VM* vm);
void builtin_write_line(VM* vm);
void builtin_write_all(VM* vm);
void builtin_file_open(VM* vm);
void builtin_file_read_line(VM* vm);
void builtin_file_close(VM* vm);
void builtin_io_read_line(VM* vm);
void builtin_io_read_all(VM* vm);
void builtin_io_read_chunk(VM* vm);
void builtin_io_read_chunk_bytes(VM* vm);
void builtin_io_read_exactly_bytes(VM* vm);
void builtin_io_write_all(VM* vm);
void builtin_io_write_bytes_all(VM* vm);
void builtin_io_copy(VM* vm);
void builtin_read_bytes(VM* vm);
void builtin_write_bytes(VM* vm);
void builtin_append_bytes(VM* vm);
void builtin_stdout_write_bytes(VM* vm);
void builtin_env_get(VM* vm);
void builtin_exists(VM* vm);
void builtin_delete(VM* vm);

// Map functions
void builtin_map_get(VM* vm);
void builtin_map_set(VM* vm);
void builtin_map_has(VM* vm);
void builtin_map_get_string(VM* vm);
void builtin_map_set_string(VM* vm);
void builtin_map_has_string(VM* vm);
void builtin_map_delete_string(VM* vm);
void builtin_map_delete(VM* vm);
void builtin_map_count(VM* vm);

// Set functions
void builtin_set_add(VM* vm);
void builtin_set_add_string(VM* vm);
void builtin_set_has(VM* vm);
void builtin_set_has_string(VM* vm);
void builtin_set_remove(VM* vm);
void builtin_set_remove_string(VM* vm);
void builtin_set_count(VM* vm);
void builtin_set_to_array(VM* vm);

// String Functions
void builtin_substring(VM* vm);
void builtin_find(VM* vm);
void builtin_split(VM* vm);
void builtin_trim(VM* vm);
void builtin_starts_with(VM* vm);
void builtin_ends_with(VM* vm);
void builtin_replace(VM* vm);

// Math Functions
void builtin_abs_int(VM* vm);
void builtin_abs_double(VM* vm);
void builtin_abs_bigint(VM* vm);
void builtin_sign_bigint(VM* vm);
void builtin_digits_bigint(VM* vm);
void builtin_is_even_bigint(VM* vm);
void builtin_is_odd_bigint(VM* vm);
void builtin_pow_bigint(VM* vm);
void builtin_gcd_bigint(VM* vm);
void builtin_lcm_bigint(VM* vm);
void builtin_mod_pow_bigint(VM* vm);
void builtin_mod_inverse_bigint(VM* vm);
void builtin_is_probable_prime_bigint(VM* vm);
void builtin_compare_bigint(VM* vm);
void builtin_abs_cmp_bigint(VM* vm);
void builtin_clamp_bigint(VM* vm);
void builtin_is_zero_bigint(VM* vm);
void builtin_is_negative_bigint(VM* vm);
void builtin_min(VM* vm);
void builtin_max(VM* vm);
void builtin_floor(VM* vm);
void builtin_ceil(VM* vm);
void builtin_round(VM* vm);
void builtin_sqrt(VM* vm);
void builtin_pow(VM* vm);
void builtin_random(VM* vm);
void builtin_random_seed(VM* vm);
void builtin_random_int(VM* vm);
void builtin_random_double(VM* vm);
void builtin_random_bigint_bits(VM* vm);
void builtin_random_bigint_range(VM* vm);
void builtin_random_fill_int(VM* vm);
void builtin_random_fill_double(VM* vm);
void builtin_random_fill_bigint_bits(VM* vm);
void builtin_random_fill_bigint_range(VM* vm);
void builtin_secure_random(VM* vm);
void builtin_secure_random_int(VM* vm);
void builtin_secure_random_double(VM* vm);
void builtin_secure_random_bigint_bits(VM* vm);
void builtin_secure_random_bigint_range(VM* vm);
void builtin_secure_random_fill_int(VM* vm);
void builtin_secure_random_fill_double(VM* vm);
void builtin_secure_random_fill_bigint_bits(VM* vm);
void builtin_secure_random_fill_bigint_range(VM* vm);

// Time/Date Functions
void builtin_time_now_millis(VM* vm);
void builtin_time_now_nanos(VM* vm);
void builtin_time_monotonic_millis(VM* vm);
void builtin_time_since_millis(VM* vm);
void builtin_utc_datetime(VM* vm);
void builtin_local_datetime(VM* vm);

// Array Functions
void builtin_sort(VM* vm);
void builtin_reverse(VM* vm);
void builtin_find_array(VM* vm);
void builtin_contains(VM* vm);
void builtin_slice(VM* vm);
void builtin_join(VM* vm);

// HTTP Client Functions
void builtin_http_get(VM* vm);
void builtin_http_get_with_headers(VM* vm);
void builtin_http_post(VM* vm);
void builtin_http_post_with_headers(VM* vm);
void builtin_http_request(VM* vm);
void builtin_http_request_head(VM* vm);
void builtin_http_request_with_options(VM* vm);
void builtin_http_request_head_with_options(VM* vm);
void builtin_http_read_request(VM* vm);
void builtin_http_write_response(VM* vm);
void builtin_log_json(VM* vm);

// Concurrency/Threading functions
void builtin_sync_channel_create(VM* vm);
void builtin_sync_channel_send(VM* vm);
void builtin_sync_channel_send_typed(VM* vm);
void builtin_sync_channel_recv(VM* vm);
void builtin_sync_channel_recv_typed(VM* vm);
void builtin_sync_channel_close(VM* vm);
void builtin_sync_shared_create(VM* vm);
void builtin_sync_shared_create_typed(VM* vm);
void builtin_sync_shared_get(VM* vm);
void builtin_sync_shared_get_typed(VM* vm);
void builtin_sync_shared_set(VM* vm);
void builtin_sync_shared_set_typed(VM* vm);
void builtin_sync_thread_spawn(VM* vm);
void builtin_sync_thread_spawn_typed(VM* vm);
void builtin_sync_thread_join(VM* vm);
void builtin_sync_thread_join_typed(VM* vm);
void builtin_sync_thread_inbox(VM* vm);
void builtin_sync_thread_outbox(VM* vm);
void builtin_sync_thread_arg_typed(VM* vm);
void builtin_sync_arc_create(VM* vm);
void builtin_sync_arc_clone(VM* vm);
void builtin_sync_arc_guard_acquire(VM* vm);
void builtin_sync_arc_guard_read(VM* vm);
void builtin_sync_arc_guard_write(VM* vm);
void builtin_sync_arc_guard_release(VM* vm);

// SQLite database functions
void builtin_sqlite_is_available(VM* vm);
void builtin_sqlite_open(VM* vm);
void builtin_sqlite_close(VM* vm);
void builtin_sqlite_exec(VM* vm);
void builtin_sqlite_query(VM* vm);
void builtin_sqlite_prepare(VM* vm);
void builtin_sqlite_bind_int(VM* vm);
void builtin_sqlite_bind_double(VM* vm);
void builtin_sqlite_bind_string(VM* vm);
void builtin_sqlite_bind_bytes(VM* vm);
void builtin_sqlite_bind_null(VM* vm);
void builtin_sqlite_reset(VM* vm);
void builtin_sqlite_clear_bindings(VM* vm);
void builtin_sqlite_changes(VM* vm);
void builtin_sqlite_last_insert_rowid(VM* vm);
void builtin_sqlite_step(VM* vm);
void builtin_sqlite_finalize(VM* vm);

// Process functions
void builtin_process_spawn(VM* vm);
void builtin_process_write_stdin(VM* vm);
void builtin_process_close_stdin(VM* vm);
void builtin_process_read_stdout(VM* vm);
void builtin_process_read_stderr(VM* vm);
void builtin_process_wait(VM* vm);
void builtin_process_kill(VM* vm);

// TCP Socket Functions
void builtin_tcp_listen(VM* vm);
void builtin_tcp_accept(VM* vm);
void builtin_tcp_connect(VM* vm);
void builtin_tcp_send(VM* vm);
void builtin_tcp_receive(VM* vm);
void builtin_tcp_close(VM* vm);

// TLS Socket Functions
void builtin_tls_is_available(VM* vm);
void builtin_tls_connect(VM* vm);
void builtin_tls_send(VM* vm);
void builtin_tls_receive(VM* vm);
void builtin_tls_close(VM* vm);

#endif
