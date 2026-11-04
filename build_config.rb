# SPDX-License-Identifier: GPL-2.0-or-later
# The interpreter rsh embeds: which gems are compiled in (docs/modules/rsh.md, "What is inside").
# eval, binding, proc-binding and objectspace are in for Ruby's metaprogramming (Kernel#eval,
# instance_eval/class_eval with a String, Binding, ObjectSpace; Rsh.eval adds the shell layer).
# Left out on purpose: socket (network goes through commands), bigint, rational, complex, task,
# debugger and the stand-alone tools.
MRuby::Build.new('rsh') do |conf|
  conf.toolchain :gcc
  conf.cc.flags = %w[-Os -fPIC -ffunction-sections -fdata-sections]
  # MRB_STR_LENGTH_MAX=0, MRB_ARY_LENGTH_MAX=0: no fixed cap on String and Array sizes (the default
  # Array cap, 131072 entries, made File.readlines of a longer file fail). MRB_NO_IO_POPEN: no
  # IO.popen and no open("|cmd"), which hand text to /bin/sh; rsh never does.
  conf.cc.defines += %w[MRB_UTF8_STRING MRB_STR_LENGTH_MAX=0 MRB_ARY_LENGTH_MAX=0 MRB_NO_IO_POPEN]
  # bin-mrbc: build-time compiler for the gems' Ruby parts (not linked into rsh).
  conf.gem core: "mruby-bin-mrbc"
  %w[compiler array-ext string-ext hash-ext enum-ext enumerator enum-lazy numeric-ext kernel-ext
     object-ext proc-ext symbol-ext range-ext compar-ext class-ext toplevel-ext struct data set
     sprintf math time strftime io dir errno pack random sleep exit error catch method metaprog
     encoding fiber eval binding proc-binding objectspace hal-posix-io hal-posix-dir].each { |g| conf.gem core: g.start_with?("hal-") ? g : "mruby-#{g}" }
end
