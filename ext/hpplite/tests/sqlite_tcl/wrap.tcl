# HPPLite on every connection a test opens: the module, loaded into one
# connection, adds itself to every later one (HPPLITE_FILES names them all).
# tester.tcl shuts SQLite down as it starts, which clears automatic
# extensions, then installs its own: HPPLite goes in again right after.
proc hpplite_load {} {
  global env
  if {![info exists env(HPPLITE_MODULE)]} return
  sqlite3 hpplite_boot :memory:
  hpplite_boot enable_load_extension 1
  hpplite_boot eval {SELECT load_extension($env(HPPLITE_MODULE))}
  hpplite_boot close
}
rename autoinstall_test_functions hpplite_orig_autoinstall
proc autoinstall_test_functions {args} { hpplite_orig_autoinstall {*}$args; hpplite_load }

# A database is its main file, its -wal and -shm (HPPLite keeps files in
# WAL) and HPPLite's -hpplite and -hpplite-users beside it: the harness's
# delete helpers, told the main file, delete them all. A -shm left behind
# makes the next database on that name read-only (delete-9.1).
proc hpplite_with_sidecars {files} {
  set out {}
  foreach f $files {
    lappend out $f
    if {![string match {*-journal} $f] && ![string match {*-wal} $f] &&
        ![string match {*-shm} $f] && ![string match {*-hpplite*} $f]} {
      lappend out $f-wal $f-shm $f-hpplite $f-hpplite-users
    }
  }
  return $out
}

# A file HPPLite cannot start on (a file format it cannot read, a schema
# rewritten whole through writable_schema) opens without it, as stock SQLite
# opens it, and the log says so: the file's tests run on, on stock SQLite
# for that connection.
proc hpplite_open {args} {
  if {![catch {uplevel 1 sqlite_orig $args} res opts]} { return $res }
  if {![string match {*hpplite: cannot start on*} $res]} { return -options $opts $res }
  set name [lindex $args 0]
  set file [lindex $args 1]
  puts "hpplite-wrap: $res"
  puts "hpplite-wrap: $name opened on $file without HPPLite"
  uplevel 1 [list sqlite_orig $name file:$file?hpplite=off -uri 1] [lrange $args 2 end]
}

# The harness's helpers, as tester.tcl (or a test) defines them:
# - "drop every table" and "empty every table": HPPLite's own tables are
#   left out, which HPPLite refuses to write (the tests' own statements are
#   untouched);
# - the delete helpers take the sidecars with the main file;
# - an open HPPLite refuses falls back to one without it.
rename proc hpplite_orig_proc
hpplite_orig_proc proc {name params body} {
  switch -- $name {
    drop_all_tables {
      set body [string map [list "name NOT LIKE 'sqliteX_%' ESCAPE 'X'" \
          "name NOT LIKE 'sqliteX_%' ESCAPE 'X' AND name NOT LIKE 'X_hppliteX_%' ESCAPE 'X'"] $body]
    }
    drop_all_tables_and_views {
      set body [string map [list "name NOT LIKE 'sqlite_%'" \
          "name NOT LIKE 'sqlite_%' AND name NOT LIKE '_hpplite_%'"] $body]
    }
    delete_all_data {
      set body [string map [list "WHERE type = 'table'\}" \
          "WHERE type = 'table' AND tbl_name NOT LIKE '_hpplite_%'\}"] $body]
    }
    do_delete_file {
      set body [string map [list "foreach filename \$args \{" \
          "foreach filename \[hpplite_with_sidecars \$args\] \{"] $body]
    }
    sqlite3 {
      set body [string map [list "set res \[uplevel 1 sqlite_orig \$args\]" \
          "set res \[uplevel 1 hpplite_open \$args\]"] $body]
    }
  }
  uplevel 1 [list hpplite_orig_proc $name $params $body]
}
hpplite_load
set argv0 [lindex $argv 0]
set argv [lrange $argv 1 end]

# Two files clean up with a loop of their own over sqlite_master that drops
# every table (alter.test, insert3.test): HPPLite's are left out of the
# listing those loops read. The file's text runs as source would run it.
set hpplite_fd [open $argv0]
set hpplite_src [read $hpplite_fd]
close $hpplite_fd
set hpplite_src [string map [list \
    "foreach tblname \[execsql {\n  SELECT name FROM sqlite_master\n   WHERE type='table' AND name NOT GLOB 'sqlite*'\n}\]" \
    "foreach tblname \[execsql {\n  SELECT name FROM sqlite_master\n   WHERE type='table' AND name NOT GLOB 'sqlite*' AND name NOT GLOB '_hpplite_*'\n}\]" \
    "foreach tab \[db eval {SELECT name FROM sqlite_master WHERE type = 'table'}\]" \
    "foreach tab \[db eval {SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT GLOB '_hpplite_*'}\]" \
  ] $hpplite_src]
info script $argv0
set hpplite_rc [catch {uplevel #0 $hpplite_src} hpplite_msg hpplite_opts]
if {$hpplite_rc == 1} { return -options $hpplite_opts $hpplite_msg }
