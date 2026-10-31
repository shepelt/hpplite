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
# the harness's "drop every table" helper would drop HPPLite's too, which
# HPPLite refuses: leave them out (the tests' own statements are untouched)
rename proc hpplite_orig_proc
hpplite_orig_proc proc {name params body} {
  if {$name eq "drop_all_tables"} {
    set body [string map [list "name NOT LIKE 'sqliteX_%' ESCAPE 'X'" \
        "name NOT LIKE 'sqliteX_%' ESCAPE 'X' AND name NOT LIKE 'X_hppliteX_%' ESCAPE 'X'"] $body]
  }
  uplevel 1 [list hpplite_orig_proc $name $params $body]
}
hpplite_load
set argv0 [lindex $argv 0]
set argv [lrange $argv 1 end]
source $argv0
