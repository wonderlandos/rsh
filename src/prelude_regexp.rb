# SPDX-License-Identifier: GPL-2.0-or-later
# rsh: the Ruby side of Regexp (the engine is in C++): MatchData, Regexp's methods and the String
# methods that take a pattern. Compiled into the binary like src/prelude.rb, but loaded only when
# first needed (Rsh.__regexp: the first Regexp made, or a stand-in in src/prelude.rb), so a script
# without patterns starts faster. The built-in String methods it wraps are kept as __rsh_plain_* by
# src/prelude.rb.
# The loops (scan, split, sub and gsub with a String) and the single matches (=~, match, match?)
# run in C++ (src/rt.cpp: __scan, __split, __sub, __eqtilde, __md, __test), which also sets $~ and
# $1..$9; what is left here calls a block or a Hash per match, on a frozen copy of the subject so
# that its UTF-8 check is done once (audit H-1, M-10).

class MatchData
  attr_reader :regexp, :string

  def initialize(regexp, string, offsets)
    @regexp = regexp
    @string = string
    @offsets = offsets
  end

  def __offsets
    @offsets
  end

  def __index(key)
    return key if key.is_a?(Integer)
    i = @regexp.__name_index(key.to_s)
    raise IndexError, "undefined group name reference: #{key}" unless i
    i
  end

  def [](key)
    i = __index(key)
    i += size if i < 0
    return nil if i < 0 || i >= size
    b = @offsets[2 * i]
    return nil if b < 0
    @string.byteslice(b, @offsets[2 * i + 1] - b)
  end

  def size
    @offsets.size / 2
  end
  alias length size

  def byteoffset(key)
    i = __index(key)
    [@offsets[2 * i], @offsets[2 * i + 1]]
  end

  def begin(key)
    b = @offsets[2 * __index(key)]
    b < 0 ? nil : Regexp.__chars(@string, b)
  end

  def end(key)
    e = @offsets[2 * __index(key) + 1]
    e < 0 ? nil : Regexp.__chars(@string, e)
  end

  def to_a
    __groups(0)
  end

  def captures
    __groups(1)
  end

  def __groups(from)
    out = []
    i = from
    n = @offsets.size
    while 2 * i < n
      b = @offsets[2 * i]
      out << (b < 0 ? nil : @string.byteslice(b, @offsets[2 * i + 1] - b))
      i += 1
    end
    out
  end

  def names
    @regexp.names
  end

  def named_captures
    h = {}
    @regexp.names.each { |n| h[n] = self[n] }
    h
  end

  def values_at(*keys)
    keys.map { |k| self[k] }
  end

  def pre_match
    @string.byteslice(0, @offsets[0])
  end

  def post_match
    @string.byteslice(@offsets[1], @string.bytesize - @offsets[1])
  end

  def to_s
    self[0]
  end

  def inspect
    "#<MatchData #{self[0].inspect}>"
  end
end

class Regexp
  class << self
    alias compile new
  end

  def self.escape(text)
    text.to_s.gsub(/[.*?+^$|(){}\[\]\\\/\-\s]/) { |c| c == "\n" ? "\\n" : c == "\t" ? "\\t" : c == " " ? "\\ " : "\\#{c}" }
  end

  def self.quote(text)
    escape(text)
  end

  def self.union(*parts)
    parts = parts.flatten
    new(parts.map { |p| p.is_a?(Regexp) ? p.source : escape(p) }.join("|"))
  end

  def self.last_match(i = nil)
    i ? $~ && $~[i] : $~
  end

  def match(str, pos = 0)
    if str.nil?
      Regexp.__set_last(nil)
      return nil
    end
    str = str.to_s
    md = __md(str, pos <= 0 ? 0 : str[0, pos].to_s.bytesize, true)
    if md && block_given?
      yield md
    else
      md
    end
  end

  def match?(str, pos = 0)
    return false if str.nil?
    str = str.to_s
    __test(str, pos <= 0 ? 0 : str[0, pos].to_s.bytesize)
  end

  def =~(str)
    if str.nil?
      Regexp.__set_last(nil)
      return nil
    end
    __eqtilde(str.to_s)
  end

  # case/when sets $~ and $1... as =~ does.
  def ===(other)
    other.is_a?(String) || other.is_a?(Symbol) ? !match(other.to_s).nil? : false
  end

  def to_s
    "(?#{options & 1 != 0 ? 'i' : ''}#{options & 4 != 0 ? 'm' : ''}#{options & 2 != 0 ? 'x' : ''}:#{source})"
  end

  def inspect
    "/#{source}/#{options & 4 != 0 ? 'm' : ''}#{options & 1 != 0 ? 'i' : ''}#{options & 2 != 0 ? 'x' : ''}"
  end

  def ==(other)
    other.is_a?(Regexp) && source == other.source && options == other.options
  end
  alias eql? ==

  def casefold?
    options & 1 != 0
  end
end

class String

  # str[/re/], str[/re/, group] (a number or a name), and slice the same: the matched text or nil.
  def [](*args)
    return __rsh_plain_aref(*args) unless args[0].is_a?(Regexp)
    md = args[0].match(self)
    md && md[args[1] || 0]
  end
  alias slice []

  def =~(other)
    raise TypeError, "wrong argument type String (expected Regexp)" if other.is_a?(String)
    other =~ self
  end

  def match(pattern, pos = 0, &block)
    pattern = Regexp.new(Regexp.escape(pattern)) if pattern.is_a?(String)
    pattern.match(self, pos, &block)
  end

  def match?(pattern, pos = 0)
    pattern = Regexp.new(Regexp.escape(pattern)) if pattern.is_a?(String)
    pattern.match?(self, pos)
  end

  # The subject the Ruby loops match on: frozen, so the engine's UTF-8 check is remembered.
  def __rsh_subject
    frozen? ? self : dup.freeze
  end

  def __rsh_replace(pattern, rep, global, &block)
    return pattern.__sub(self, rep.to_s, global) unless (block && rep.nil?) || rep.is_a?(Hash)
    subj = __rsh_subject
    out = ""
    pos = 0
    last = nil
    bytes = subj.bytesize
    while pos <= bytes
      md = pattern.__md(subj, pos, false)
      break unless md
      offs = md.__offsets
      last = md
      out << subj.byteslice(pos, offs[0] - pos)
      out << (rep.is_a?(Hash) ? rep[md[0]] : block.call(md[0])).to_s
      if offs[1] == offs[0]
        out << subj.byteslice(offs[1], 1).to_s if offs[1] < bytes
        pos = offs[1] + 1
      else
        pos = offs[1]
      end
      break unless global
    end
    out << subj.byteslice(pos, bytes - pos).to_s if pos < bytes
    Regexp.__set_last(last)
    out
  end

  def sub(pattern, rep = nil, &block)
    return (rep.nil? ? __rsh_plain_sub(pattern, &block) : __rsh_plain_sub(pattern, rep)) unless pattern.is_a?(Regexp)
    __rsh_replace(pattern, rep, false, &block)
  end

  def gsub(pattern, rep = nil, &block)
    return (rep.nil? ? __rsh_plain_gsub(pattern, &block) : __rsh_plain_gsub(pattern, rep)) unless pattern.is_a?(Regexp)
    __rsh_replace(pattern, rep, true, &block)
  end

  def sub!(pattern, rep = nil, &block)
    r = sub(pattern, rep, &block)
    r == self ? nil : replace(r)
  end

  def gsub!(pattern, rep = nil, &block)
    r = gsub(pattern, rep, &block)
    r == self ? nil : replace(r)
  end

  def scan(pattern, &block)
    pattern = Regexp.new(Regexp.escape(pattern)) if pattern.is_a?(String)
    return pattern.__scan(self) unless block
    subj = __rsh_subject
    pos = 0
    bytes = subj.bytesize
    while pos <= bytes
      md = pattern.__md(subj, pos, false)
      break unless md
      offs = md.__offsets
      block.call(md.size > 1 ? md.captures : md[0])
      pos = offs[1] == offs[0] ? offs[1] + 1 : offs[1]
    end
    self
  end

  def split(pattern = nil, limit = 0)
    return __rsh_plain_split(pattern, limit) unless pattern.is_a?(Regexp)
    pattern.__split(self, limit)
  end

  def index(pattern, pos = 0)
    return __rsh_plain_index(pattern, pos) unless pattern.is_a?(Regexp)
    md = pattern.match(self, pos)
    md && md.begin(0)
  end

  def start_with?(*prefixes)
    prefixes.any? do |p|
      if p.is_a?(Regexp)
        md = p.match(self)
        md && md.begin(0) == 0
      else
        p = p.to_s
        self[0, p.size] == p
      end
    end
  end
end

