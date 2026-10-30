<?php
// auto_prepend_file: puts HPPLite into PHP's SQLite before WordPress opens its
// database (the SQLite Database Integration plugin opens a plain path with PDO;
// HPPLITE_FILES names it). WordPress and the plugin are untouched.
if (getenv('HPPLITE_EXT')) { (new Pdo\Sqlite('sqlite::memory:'))->loadExtension(getenv('HPPLITE_EXT')); }
