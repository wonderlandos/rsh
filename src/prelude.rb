# SPDX-License-Identifier: GPL-2.0-or-later
# rsh: the Ruby half of the shell layer, compiled into the binary. Command lines call
# __rsh_cmd / __rsh_cap (the reader writes those calls); Rsh::Status is $?, Rsh::Failed is what
# an unhandled failure raises. Also: ENV, JSON, Regexp's Ruby side, Process, Etc, require.

class Interrupt < Exception; end unless Object.const_defined?(:Interrupt)
class LoadError < ScriptError; end unless Object.const_defined?(:LoadError)

module Rsh
  # A mistake in a command line itself: a nil value, a file pattern with no match, an unset $NAME.
  class Error < StandardError; end

  # A command failed and the script did not handle it (no if/unless, no ||, not relaxed!).
  class Failed < StandardError
    attr_reader :status, :argv, :stderr

    def initialize(status, argv, stderr)
      @status = status
      @argv = argv
      @stderr = stderr
      how = status.termsig ? "was stopped by signal #{status.termsig}" : "exited #{status.exitstatus}"
      super("#{argv[0]} #{how}")
    end
  end

  class Status
    attr_reader :exitstatus, :statuses, :termsig

    def initialize(code, statuses, sig)
      @exitstatus = code
      @statuses = statuses
      @termsig = sig
    end

    def success?
      @exitstatus == 0
    end

    def signaled?
      !@termsig.nil?
    end

    def to_i
      @exitstatus
    end

    def ==(other)
      other.is_a?(Integer) ? @exitstatus == other : equal?(other)
    end

    def to_s
      "exit #{@exitstatus}"
    end

    def inspect
      "#<Rsh::Status #{@exitstatus}#{@statuses.size > 1 ? " #{@statuses.inspect}" : ''}>"
    end
  end

  @relaxed = false

  def self.relaxed?
    @relaxed
  end

  def self.relaxed=(on)
    @relaxed = on ? true : false
  end

  def self.nullglob=(on)
    __nullglob(on ? true : false)
  end

  def self.__vals(vals)
    vals.map do |x|
      case x
      when String then x
      when Proc then x
      when Array
        x.flatten.map do |e|
          raise Rsh::Error, "a value in a command is nil" if e.nil?
          e.to_s
        end
      when nil then raise Rsh::Error, "a value in a command is nil (an empty argument is \"\")"
      else x.to_s
      end
    end
  end

  # r: [code, statuses, signal, raise?, argv, error tail, interrupted?, output]
  def self.__finish(r)
    st = Status.new(r[0], r[1], r[2])
    __set_status(st)
    raise Interrupt, "interrupted" if r[6]
    raise Failed.new(st, r[4], r[5]) if r[3]
    st
  end

  # Runs one program with these exact arguments (no parsing, no patterns).
  def self.run(*argv)
    __finish(__argv(argv.flatten.map(&:to_s), 0)).success?
  end

  # Runs a text with the shell layer: its command lines run, as in a file. Without a binding it
  # runs at the top level (self is main, no local variables of the caller); with one
  # (Rsh.eval(text, binding)) it sees that binding's self and local variables. Kernel#eval is
  # plain Ruby and never runs a command line.
  def self.eval(text, bind = nil, file = "(rsh eval)", line = 1)
    locals = bind ? bind.local_variables : []
    code = __translate(text.to_s, file.to_s, line, locals)
    bind ? bind.eval(code, file.to_s, line) : __load(code, file.to_s, line)
  end

  # The Ruby text rsh runs for this text (what Rsh.eval would run).
  def self.translate(text, file = "(rsh eval)", line = 1)
    __translate(text.to_s, file.to_s, line, [])
  end
end

def relaxed!(on = true)
  Rsh.relaxed = on
end

def __rsh_cmd(i, v, cond = false)
  flags = (cond ? 1 : 0) | (Rsh.relaxed? ? 4 : 0)
  Rsh.__finish(Rsh.__exec(i, Rsh.__vals(v), flags)).success?
end

# A capture gives the output as a String; its status is in $?.
def __rsh_cap(i, v)
  r = Rsh.__exec(i, Rsh.__vals(v), 2 | (Rsh.relaxed? ? 4 : 0))
  Rsh.__finish(r)
  r[7]
end

def __rsh_cd_do(i, v)
  back = Dir.pwd
  __rsh_cmd(i, v)
  begin
    yield
  ensure
    Rsh.__chdir(back)
  end
end

module Kernel
  # Arguments only: system("ls", "-l", dir). One string with spaces would need a shell, which
  # rsh never uses; write the command line itself instead.
  def system(*argv)
    argv = argv.flatten.map(&:to_s)
    if argv.size == 1 && argv[0].include?(" ")
      raise Rsh::Error, "system takes separate arguments: system(\"ls\", \"-l\") or write the command line"
    end
    r = Rsh.__argv(argv, 1)
    st = Rsh.__finish(r)
    st.exitstatus == 127 ? nil : st.success?
  end

  def `(_text)
    raise Rsh::Error, "this capture was built at run time; write it in the script as `...` so values stay single arguments"
  end

  # rsh_methods :deploy, :restart: these bare words are Ruby method calls, not commands (methods a
  # script makes at run time, with method_missing or define_method over a list). The reader sees
  # the literal names in the file; at run time the call only adds them for Rsh.eval texts.
  # Kernel#warn (the built-in interpreter has none): the messages on standard error.
  def warn(*msgs, **)
    msgs.flatten.each { |m| $stderr.puts(m.to_s) }
    nil
  end

  # Kernel#abort: the message on standard error, then exit 1.
  def abort(msg = nil)
    $stderr.puts(msg.to_s) if msg
    exit 1
  end

  def rsh_methods(*names)
    Rsh.__declare(names.flatten.map(&:to_s))
    nil
  end

  # ruby do ... end, ruby { ... }: no bare word inside is a command line (the reader decides that;
  # here it only runs the block). ruby! does the same for the rest of the file.
  def ruby
    yield
  end

  def ruby!
    nil
  end

  def require(name)
    return false if %w[json set time etc].include?(name.to_s)
    raise LoadError, "cannot load #{name}: rsh has its library built in (json, set, time); load your own files with require_relative"
  end

  def require_relative(path)
    Rsh.__require(path.to_s, true)
  end

  def load(path)
    Rsh.__require(path.to_s, false)
  end
end

class File
  def self.write(path, data, *_opts)
    text = data.to_s
    File.open(path.to_s, "w") { |f| f.write(text) }
    text.bytesize
  end

  def self.readlines(path, *_opts)
    File.open(path.to_s) { |f| f.each_line.to_a }
  end
end

ENV = Object.new
class << ENV
  def [](key)
    Rsh.__getenv(key.to_s)
  end

  def []=(key, value)
    Rsh.__setenv(key.to_s, value.nil? ? nil : value.to_s)
    value
  end

  def fetch(key, *default)
    v = self[key]
    return v if v
    return yield(key) if block_given?
    return default[0] unless default.empty?
    raise KeyError, "key not found: #{key}"
  end

  def key?(key)
    !self[key].nil?
  end
  alias include? key?
  alias has_key? key?

  def delete(key)
    v = self[key]
    Rsh.__setenv(key.to_s, nil)
    v
  end

  def to_h
    Rsh.__env_hash
  end
  alias to_hash to_h

  def each(&block)
    to_h.each(&block)
  end

  def keys
    to_h.keys
  end

  def values
    to_h.values
  end

  def update(hash)
    hash.each { |k, v| self[k] = v }
    self
  end
  alias merge! update

  def inspect
    to_h.inspect
  end
end

module Process
  def self.pid
    Rsh.__pid
  end

  def self.kill(sig, *pids)
    pids.each { |p| Rsh.__kill(sig.to_s, p) }
    pids.size
  end
end

module Etc
  def self.nprocessors
    Rsh.__nproc
  end
end

class Dir
  def self.glob(pattern)
    Array(pattern).flat_map { |p| Rsh.__glob(p) }
  end

  def self.[](*patterns)
    glob(patterns)
  end
end

# ---- JSON ---------------------------------------------------------------------------------------

module JSON
  class ParserError < StandardError; end

  def self.parse(text, symbolize_names: false)
    Rsh.__json_parse(text.to_s, symbolize_names)
  end

  def self.generate(obj)
    obj.to_json
  end

  def self.dump(obj)
    obj.to_json
  end

  def self.pretty_generate(obj, indent = "  ", level = 0)
    pad = indent * (level + 1)
    case obj
    when Hash
      return "{}" if obj.empty?
      body = obj.map { |k, v| "#{pad}#{k.to_s.to_json}: #{pretty_generate(v, indent, level + 1)}" }
      "{\n#{body.join(",\n")}\n#{indent * level}}"
    when Array
      return "[]" if obj.empty?
      body = obj.map { |v| "#{pad}#{pretty_generate(v, indent, level + 1)}" }
      "[\n#{body.join(",\n")}\n#{indent * level}]"
    else
      obj.to_json
    end
  end
end

class Object
  def to_json(*)
    to_s.to_json
  end
end

class String
  def to_json(*)
    Rsh.__json_str(self)
  end
end

class Symbol
  def to_json(*)
    to_s.to_json
  end
end

class Integer
  def to_json(*)
    to_s
  end
end

class Float
  def to_json(*)
    raise JSON::ParserError, "#{self} is not valid JSON" if nan? || infinite?
    to_s
  end
end

class NilClass
  def to_json(*)
    "null"
  end
end

class TrueClass
  def to_json(*)
    "true"
  end
end

class FalseClass
  def to_json(*)
    "false"
  end
end

class Array
  def to_json(*)
    "[#{map(&:to_json).join(',')}]"
  end
end

class Hash
  def to_json(*)
    "{#{map { |k, v| "#{k.to_s.to_json}:#{v.to_json}" }.join(',')}}"
  end
end

# ---- Regexp: the engine is in C++, its Ruby side (MatchData, Regexp's methods, the String methods
# that take a pattern) is in src/prelude_regexp.rb, loaded the first time a Regexp is made or one of
# the methods below is called. A script without patterns does not pay for it at start-up. The
# String methods that also take a plain String (sub, gsub, split, index, [], start_with?) need no
# stand-in: until a Regexp exists, their built-in versions are the right ones.

class Regexp
  IGNORECASE = 1
  EXTENDED = 2
  MULTILINE = 4

  def self.compile(*args) = (Rsh.__regexp; compile(*args))
  def self.escape(*args) = (Rsh.__regexp; escape(*args))
  def self.quote(*args) = (Rsh.__regexp; quote(*args))
  def self.union(*args) = (Rsh.__regexp; union(*args))
  def self.last_match(*args) = (Rsh.__regexp; last_match(*args))
end

class MatchData; end

class String
  alias __rsh_plain_sub sub
  alias __rsh_plain_gsub gsub
  alias __rsh_plain_split split
  alias __rsh_plain_index index
  alias __rsh_plain_aref []

  def =~(*args) = (Rsh.__regexp; self =~ args[0])
  def match(*args, &block) = (Rsh.__regexp; match(*args, &block))
  def match?(*args) = (Rsh.__regexp; match?(*args))
  def scan(*args, &block) = (Rsh.__regexp; scan(*args, &block))
end

class Symbol
  def =~(other)
    to_s =~ other
  end

  def match(*args)
    to_s.match(*args)
  end
end

# instance_eval and class_eval with a String: the interpreter would leave the receiver as the
# place where `def` and `class` go for the rest of the calling scope (a top-level `def` after
# A.class_eval("...") landed in A). Called from here, the text gets its own scope: it does not see
# the caller's local variables (pass values in with #{} or use the block form, which does).
class Module
  # Queries the built-in interpreter leaves out.
  def private_method_defined?(name, inherit = true) = private_instance_methods(inherit).include?(name.to_sym)
  def public_method_defined?(name, inherit = true) = public_instance_methods(inherit).include?(name.to_sym)
  def protected_method_defined?(name, inherit = true) = protected_instance_methods(inherit).include?(name.to_sym)

  alias __rsh_class_eval class_eval

  def class_eval(*__rsh_args, &__rsh_block)
    return __rsh_class_eval(&__rsh_block) if __rsh_block
    __rsh_class_eval(*__rsh_args)
  end

  def module_eval(*__rsh_args, &__rsh_block)
    return __rsh_class_eval(&__rsh_block) if __rsh_block
    __rsh_class_eval(*__rsh_args)
  end
end

class BasicObject
  alias __rsh_instance_eval instance_eval

  def instance_eval(*__rsh_args, &__rsh_block)
    return __rsh_instance_eval(&__rsh_block) if __rsh_block
    __rsh_instance_eval(*__rsh_args)
  end
end

class Data
  # A copy with some members changed (Data#with).
  def with(**changes) = changes.empty? ? self : self.class.new(**to_h.merge(changes))
end
