# SPDX-License-Identifier: GPL-2.0-or-later
# rsh: the Ruby side of signals, trap and at_exit, Dir.home and Process.kill (0.1.4, audits H-2,
# H-3, M-6). Compiled in like src/prelude.rb, but loaded the first time one of these is used
# (Rsh.__more, src/main.cpp), so a script that uses none of them does not pay for it at start-up.
# The C side (Rsh.__sig_take, __trap, __signal_exc, __home_self, __ppid) is src/main.cpp.

module Signal
  # The table is made on first use (start-up does not pay for it).
  def self.__list
    @list ||= { "EXIT" => 0, "HUP" => 1, "INT" => 2, "QUIT" => 3, "ILL" => 4, "TRAP" => 5, "ABRT" => 6, "IOT" => 6,
                "BUS" => 7, "FPE" => 8, "KILL" => 9, "USR1" => 10, "SEGV" => 11, "USR2" => 12, "PIPE" => 13,
                "ALRM" => 14, "TERM" => 15, "STKFLT" => 16, "CHLD" => 17, "CLD" => 17, "CONT" => 18, "STOP" => 19,
                "TSTP" => 20, "TTIN" => 21, "TTOU" => 22, "URG" => 23, "XCPU" => 24, "XFSZ" => 25, "VTALRM" => 26,
                "PROF" => 27, "WINCH" => 28, "IO" => 29, "POLL" => 29, "PWR" => 30, "SYS" => 31 }
  end

  def self.list = __list.dup

  def self.signame(n) = __list.key(n)

  # A signal number from 15, "TERM", "SIGTERM" or :TERM; ArgumentError for a name no system has.
  def self.__num(sig)
    return sig if sig.is_a?(Integer) && sig >= 0 && sig < 65
    raise ArgumentError, "bad signal type #{sig.class}" unless sig.is_a?(String) || sig.is_a?(Symbol)
    name = sig.to_s
    return name.to_i if !name.empty? && name.bytes.all? { |b| b >= 48 && b <= 57 } && name.to_i < 65
    name = name[3..] if name.start_with?("SIG")
    __list.fetch(name) { raise ArgumentError, "unsupported signal 'SIG#{name}'" }
  end

  # trap("TERM") { ... }, trap(:INT, "IGNORE"), trap("HUP", "DEFAULT"); trap("EXIT") { } runs at the
  # end, after the at_exit blocks. Returns the handler that was set before.
  def self.trap(sig, command = nil, &block)
    n = __num(sig)
    raise ArgumentError, "can't trap reserved signal: SIG#{signame(n)}" if [9, 19].include?(n)
    h = block || command
    h = nil if %w[DEFAULT SIG_DFL SYSTEM_DEFAULT].include?(h)
    h = "IGNORE" if ["SIG_IGN", ""].include?(h)
    h = proc { exit } if h == "EXIT" && n != 0
    raise ArgumentError, "a trap handler is a block, \"IGNORE\" or \"DEFAULT\"" unless h.nil? || h == "IGNORE" || h.respond_to?(:call)
    old = Rsh.__traps[n]
    Rsh.__traps[n] = h
    Rsh.__trap(n, h.nil? ? 0 : h == "IGNORE" ? 1 : 2) if n > 0
    old.nil? ? "DEFAULT" : old
  end
end

module Rsh
  @traps = {}
  @at_exit = []

  def self.__traps = @traps
  def self.__at_exit = @at_exit

  # The safe point: the trap blocks of the signals that came, or the exception that ends the script.
  def self.__signals
    while (n = __sig_take) > 0
      h = @traps[n]
      raise __signal_exc(n) unless h.respond_to?(:call)
      h.arity == 0 ? h.call : h.call(n)
    end
  end

  # Called once when the script ends (and when an interactive session ends): the at_exit blocks,
  # last first, then a trap("EXIT") block. An `exit N` in one sets the status.
  def self.__run_at_exit(status)
    blocks = @at_exit.reverse
    blocks << @traps[0] if @traps[0].respond_to?(:call)
    @at_exit = []
    blocks.each do |b|
      begin
        b.call
      rescue SystemExit => e
        status = e.status
      rescue Exception => e # rubocop:disable Lint/RescueException
        $stderr.puts("rsh: at_exit: #{e.message} (#{e.class})")
        status = 1 if status == 0
      end
    end
    status
  end
end

module Kernel
  # at_exit { ... }: run when the script ends, by any way but exit! (last registered first).
  def at_exit(&block)
    raise ArgumentError, "called without a block" unless block
    Rsh.__at_exit << block
    block
  end

  def trap(sig, command = nil, &block)
    Signal.trap(sig, command, &block)
  end
end

module Process
  # Every signal name of the system (Signal.list), a number, a Symbol; an unknown name is an
  # ArgumentError and nothing is sent (it sent signal 0 and said it had, audit M-6).
  def self.kill(sig, *pids)
    n = Signal.__num(sig)
    pids.each { |p| Rsh.__kill(n.to_s, p) }
    pids.size
  end

  def self.ppid = Rsh.__ppid
end

class Dir
  # Dir.home: $HOME; Dir.home("user") from /etc/passwd, as ~user is read (audit H-3). With HOME
  # unset or empty the account of the person running rsh, from /etc/passwd.
  def self.home(user = nil)
    if user.nil?
      h = ENV["HOME"]
      return h unless h.nil? || h.empty?
      h = Rsh.__home_self
      raise ArgumentError, "couldn't find home directory: $HOME is not set" unless h
      return h
    end
    h = Rsh.__home(user.to_s)
    raise ArgumentError, "user #{user} doesn't exist" unless h
    h
  end
end

class File
  # File.stat / File.lstat (0.9.0): what `stat` shows, for scripts that check a file in Ruby. The
  # numbers come from Rsh.__stat (src/rt.cpp); Errno errors as in Ruby.
  class Stat
    include Comparable

    def initialize(path, follow = true)
      @a = Rsh.__stat(path.to_s, follow)
    end

    def mode = @a[0]
    def size = @a[1]
    def uid = @a[2]
    def gid = @a[3]
    def nlink = @a[4]
    def ino = @a[5]
    def dev = @a[6]
    def atime = Time.at(@a[7], @a[8])
    def mtime = Time.at(@a[9], @a[10])
    def ctime = Time.at(@a[11], @a[12])
    def rdev = @a[13]
    def blksize = @a[14]
    def blocks = @a[15]
    def size? = size.zero? ? nil : size
    def zero? = size.zero?
    def __type = mode & 0o170000
    def file? = __type == 0o100000
    def directory? = __type == 0o040000
    def symlink? = __type == 0o120000
    def pipe? = __type == 0o010000
    def socket? = __type == 0o140000
    def chardev? = __type == 0o020000
    def blockdev? = __type == 0o060000
    def setuid? = mode & 0o4000 != 0
    def setgid? = mode & 0o2000 != 0
    def sticky? = mode & 0o1000 != 0
    def owned? = uid == Process.euid
    def world_writable? = mode & 0o002 != 0 ? mode & 0o777 : nil
    def world_readable? = mode & 0o004 != 0 ? mode & 0o777 : nil
    def <=>(other) = mtime <=> other.mtime

    def ftype
      { 0o100000 => "file", 0o040000 => "directory", 0o120000 => "link", 0o010000 => "fifo",
        0o140000 => "socket", 0o020000 => "characterSpecial", 0o060000 => "blockSpecial" }.fetch(__type, "unknown")
    end

    def inspect = "#<File::Stat mode=0#{mode.to_s(8)} size=#{size} uid=#{uid} gid=#{gid}>"
  end

  def self.stat(path) = Stat.new(path, true)
  def self.lstat(path) = Stat.new(path, false)
  def self.ftype(path) = Stat.new(path, false).ftype
end

class Time
  # What `require "time"` gives in Ruby, for the ISO 8601 forms scripts write and read (0.9.0):
  # Time#iso8601 / #xmlschema, Time.iso8601 and Time.parse of "2024-01-02", "2024-01-02 03:04",
  # "2024-01-02T03:04:05.123+01:00", "...Z". Another form is ArgumentError (Time.parse in Ruby
  # knows many more; rsh says so instead of guessing).
  def iso8601(digits = 0)
    frac = ""
    if digits > 0
      us = format("%06d", usec)
      frac = "." + (digits <= 6 ? us[0, digits] : us + "0" * (digits - 6))
    end
    zone = utc? ? "Z" : strftime("%z").sub(/(\d\d)(\d\d)\z/, '\1:\2')
    strftime("%Y-%m-%dT%H:%M:%S") + frac + zone
  end

  def xmlschema(digits = 0) = iso8601(digits)

  def self.parse(text)
    m = /\A\s*(\d{4})-(\d\d)-(\d\d)(?:[T ](\d\d):(\d\d)(?::(\d\d)(?:\.(\d+))?)?)?\s*(Z|[+-]\d\d:?\d\d)?\s*\z/.match(text.to_s)
    raise ArgumentError, "no time information in #{text.inspect} (rsh reads ISO 8601: 2024-01-02T03:04:05+01:00)" unless m
    parts = [m[1], m[2], m[3], m[4] || 0, m[5] || 0, m[6] || 0].map(&:to_i)
    us = m[7] ? (m[7] + "000000")[0, 6].to_i : 0
    zone = m[8]
    return Time.local(*parts, us) unless zone
    t = Time.utc(*parts, us)
    return t if zone == "Z"
    sign = zone[0] == "-" ? -1 : 1
    digits = zone.delete(":")
    off = sign * (digits[1, 2].to_i * 3600 + digits[3, 2].to_i * 60)
    (t - off).localtime
  end

  def self.iso8601(text) = parse(text)
end
