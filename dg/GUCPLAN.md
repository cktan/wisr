# GUC plan: settings by who writes them and who reads them

This plan moves the GUC variables into the control blocks
([`CODE.md`](CODE.md) §3, `src/include/utils/control_block.h`).
It moves a variable only when a change needs it. It never moves
all of them in one operation.

## 1. State today

- The build has 210 settings: 11 `INTERNAL`, 23 `POSTMASTER`,
  36 `SIGHUP`, 2 `BACKEND`, 2 `SU_BACKEND`, 31 `SUSET` and 105
  `USERSET`.
- Each thread builds its own table and variables with
  `InitializeGUCOptions()`.
- A new session gets its spawner's non-default settings as text
  (`write_nondefault_variables_to()` and
  `read_nondefault_variables_from()`).
- Each session re-reads the config file on `INTERRUPT_RELOAD`
  (`ProcessConfigFile()` in `postgres.c`). The postmaster
  signals the sessions to do so.
- The `POSTMASTER` and `INTERNAL` settings are in `pcb_t`
  already (`publish_postmaster_settings()` in `guc_tables.c`).
- The six file-logger settings are in `pcb_t` too, guarded by
  `shmem->log.mutex` (`publish_log_file_settings()`).

## 2. The three groups

| Group | Block | Who writes | Who reads |
|---|---|---|---|
| 1 | `pcb` | the postmaster | the postmaster; any thread, for a constant |
| 2 | `shmem` | the postmaster, on a reload | the postmaster and the other threads |
| 3 | `tcb`, in `tcb->guc` | each thread | that thread alone |

- **Group 1** is what the postmaster reads and writes. A
  constant, fixed after startup, may be read by other threads.
- **Group 2** changes after the first thread starts. It lives
  in `shmem_t`, which already holds such state (the TLS
  context, the roles). Guard each member with an atomic, or
  with an immutable snapshot, a `std::shared_ptr<const T>`,
  that the postmaster swaps under a mutex. A reader copies the
  pointer under the mutex and cannot change what it points to.
  Only the function that publishes the settings swaps the
  pointer, so the types keep the postmaster the only writer.
- **Group 3** is every setting a session can `SET`. It lives
  in a `tcb->guc` struct, one field for each variable.
- **Classify by reader, not by context.** A `SIGHUP` setting
  goes to group 1 if only the postmaster reads it. It goes to
  group 2 if a session reads it.

### 2.1 Where each setting goes

| Group | Settings |
|---|---|
| 1 | The 23 `POSTMASTER` and 11 `INTERNAL` settings (done). The `SIGHUP` settings only the postmaster reads: `ssl_ca_file`, `ssl_cert_file`, `ssl_crl_dir`, `ssl_crl_file`, `ssl_dh_params_file`, `ssl_key_file`, `ssl_groups`, `ssl_min_protocol_version`, `ssl_max_protocol_version`, `ssl_passphrase_command`, `ssl_passphrase_command_supports_reload`, `send_abort_for_crash`, `send_abort_for_kill`. |
| 2 | `log_line_prefix`, `syslog_facility`, `syslog_ident`, `syslog_sequence_numbers`, `syslog_split_messages` (`elog.c` reads them in every thread). The six file-logger settings (now in `pcb`: they move to `shmem`, next to `log.mutex`, when a change touches them). `ssl`, `log_hostname`, `authentication_timeout`, `allow_alter_system`, `oauth_validator_libraries`. |
| 3 | The 105 `USERSET`, 31 `SUSET`, 2 `BACKEND` and 2 `SU_BACKEND` settings. `log_connections`, `log_disconnections`, `post_auth_delay` and `ignore_system_indexes` are here because the session reads them at connect or disconnect. |
| No reader found | `ssl_ciphers`, `ssl_prefer_server_ciphers`, `ssl_tls13_ciphers`, `krb_server_keyfile`, `krb_caseins_users`, `gss_accept_delegation`, `pre_auth_delay`. They are compiled out, not removed ([`CODE.md`](CODE.md) §7): keep them, in group 1. |

- The `SIGHUP` and `BACKEND` rows come from a search for
  readers. Check each setting again when it moves.
  `authentication_timeout`, for one, is read in `postinit.c`
  and in `be-secure-openssl.c`.

## 3. Why this is correct

- **Group 1 has no data race.** The postmaster writes a
  constant before it starts a thread. The creation of a thread
  orders that write before every read in it.
- **Group 2 is safe because of its guard.** The postmaster
  rewrites it while threads read it. An atomic covers one
  value. A snapshot covers a set that must agree, such as the
  `ssl_*` settings.
- **Group 3 matches PostgreSQL.** A forked backend inherits the
  postmaster's GUC state, with its source and `reset_val`. A
  thread that starts with a copy of its spawner's settings
  does the same.
- **The copy runs in the spawner's thread, before the child
  starts.** If the child read the postmaster's block from its
  own thread, it would race with a reload. A reload and a
  spawn run in the postmaster's thread, so a child sees the
  old settings or the new ones, never a mix.
  `session_thread_start()` works this way today.
- **A session needs no reload.** A `SIGHUP` setting cannot be
  `SET` by a session, so its own copy only repeats the
  postmaster's. A `USERSET` or `SUSET` setting is private to
  the session once it is set.

## 4. Behavior changes

- A reload no longer reaches a running session. Only a new
  connection sees the new defaults.
- `RESET` returns a setting to its value at session start.
- `SHOW` of a group 1 or group 2 setting in a session shows
  the value at session start.
- `pg_reload_conf()` still works, but it acts on the postmaster
  only. `pg_conf_load_time` and `pg_file_settings` become
  simpler, as only the postmaster applies the file.
- A thread that digres did not start (`session_thread_adopt()`,
  sm's index builders) has no spawner. It keeps the compiled-in
  defaults, as now.
- Record these changes in `DG-vs-PG.md`.

## 5. How a variable moves

The text copy at thread start stays, for every setting, moved
or not. This keeps one variable's move small.

1. Add the field to the right block, with a comment that names
   the file where the variable was defined.
2. Bind the table entry. For group 3,
   `build_guc_variables()` sets the entry's `variable` pointer
   to `&tcb->guc.name` as it builds the thread's table.
   `tcb_init()` runs first, and the table lives as long as the
   `tcb`.
3. Change the read sites to read the field.
4. Remove the old `thread_local` variable, and its `extern`.
5. Run the test suite.

- A group 2 variable moves with the same steps, but its field
  is in `shmem_t` and it needs no table binding. The
  postmaster copies it from its GUC variable into `shmem` with a
  `publish_*()` function, at start and on each reload, and
  every reader reads `shmem`.
- The copy of the whole block, with the table metadata (source,
  `scontext`, `srole`, `reset_val` and the non-default list),
  would replace the text copy. It is possible only after every
  group 3 variable has moved. Decide then whether it is worth
  doing.
- Moving a group 3 variable adds a load of the `tcb` pointer
  on each read. Ask before you move a variable the planner or
  the executor reads in a hot loop ([`CLAUDE.md`](../CLAUDE.md)).

## 6. Steps

1. **Confirm the starting state.**
   - Run `DEBUG=1 make -C dg -j4 test` and save the log.
   - Check that every caller of `InitializeGUCOptions()` has a
     `tcb` first: `session_thread_main()`,
     `session_thread_adopt()`, `parse_tool.c` and the
     postmaster.
2. **Add an empty `tcb->guc` and the binding.** No variable
   moves yet.
3. **Move a variable when a change needs it**, as section 5
   says. Start with the first setting a change touches.
4. **Stop the sessions reloading, only when the gate below is
   met.**
   - **Gate:** every `SIGHUP` setting that a session reads is
     in `shmem` (group 2) and read from there. Until then a
     session must keep reloading. Without the gate, a session
     keeps the old value of a setting for its whole life:
     after `allow_alter_system = off`, a held session could
     still run `ALTER SYSTEM`, and `log_line_prefix` would stay
     old.
   - **Check the gate with the compiler:** make the variable of
     each `SIGHUP` setting `static` in `guc_tables.c`, as the
     `POSTMASTER` ones are. A file that still names it fails to
     build.
   - Remove the `INTERRUPT_RELOAD` handler and the
     `ConfigReloadPending` check in `postgres.c`.
   - Remove `SignalChildren(SIGHUP)` in `postmaster.c`.
   - Remove the code in `ProcessConfigFileInternal()` that
     skips the load time in a session thread.
   - Add a test: a held session keeps the old value of a
     `USERSET` setting after a reload, and a new session gets
     the new one. Add a second test: after
     `allow_alter_system = off` and a reload, a held session
     cannot run `ALTER SYSTEM`. Add both to `dg/test` and
     `server.sh`.
   - Change the `server.sh` check that expects the same load
     time in a held session and a new one.
5. **Update the documents once**: [`CODE.md`](CODE.md) §3, the
   header of `control_block.h`, [`WORKNOTE.md`](WORKNOTE.md),
   `DG-vs-PG.md` and `src/backend/utils/misc/README`.

## 7. Order

- Moving the group 2 settings comes before the stop of session
  reloads, not after. The stop is the last code step.
- The group 2 settings to move before the gate: `ssl`,
  `log_hostname`, `log_line_prefix`, `syslog_facility`,
  `syslog_ident`, `syslog_sequence_numbers`,
  `syslog_split_messages`, `authentication_timeout`,
  `allow_alter_system` and `oauth_validator_libraries`. The
  six file-logger settings are read from `pcb` under
  `log.mutex` already, so they do not block the gate.

## 8. Target: three settings a day

- **Why:** so we can remove SIGHUP from the connection threads.
  A session may stop reloading only when no `SIGHUP` setting
  it reads is still its own copy. Each setting moved to
  `shmem` brings that day closer.
- **Rate:** move three settings a day, each by section 5, each
  with its own test run and its own commit.
- **Order:** by reader, so one day touches few files. Settings
  that `elog.c` reads go together.
- **Finish:** the day after the last group 2 setting moves, stop
  the session reloads (step 4 of section 6), which needs the
  gate.
- **Group 3:** the same rate applies after that, in the order
  changes touch the variables.

| Day | Settings | Reader |
|---|---|---|
| done | `log_line_prefix` | `elog.c` |
| 1, done | `log_hostname`, `authentication_timeout`, `allow_alter_system` | `backend_startup.c`, `postinit.c`, `be-secure-openssl.c`, ALTER SYSTEM |
| 2 | `syslog_facility`, `syslog_ident`, `syslog_sequence_numbers` | `elog.c` |
| 3 | `syslog_split_messages`, `ssl`, `oauth_validator_libraries` | `elog.c`, `postmaster.c`, `auth-oauth.c` |
| 4 | Stop the session reloads; update the documents | `postgres.c`, `postmaster.c` |

- Move a setting early if a change touches it first. Keep the
  table current: mark a setting done when it is committed.
