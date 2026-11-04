# SPDX-License-Identifier: GPL-2.0-or-later
# rsh: the shell things as Ruby values (0.9.0; docs/design.md S8, docs/rsh.md "Sh"). `Sh` and the
# Ruby-standard names FileUtils, Find, Shellwords and Dir.mktmpdir are built in: no require, and
# this file is compiled in like src/prelude.rb but loaded the first time one of them is used
# (Rsh.__std, src/main.cpp), so start-up does not pay for it. No program is started: each helper
# works on the file system itself and gives a Ruby value (an Array of paths, a String, true/false),
# so nothing is split again. The option set is small on purpose; anything more is the real program
# (`find . -printf ...` is still a command line).

module Sh
  extend self

  def self.respond_to_missing?(_name, _priv = false) = false

  # A name pattern (* ? [a-z] [!a-z] {a,b}) as a Regexp for one file name.
  def __name_re(pat)
    out = +""
    i = 0
    depth = 0
    while i < pat.size
      c = pat[i]
      case c
      when "*" then out << ".*"
      when "?" then out << "."
      when "["
        j = pat.index("]", i + 1)
        if j
          out << "[" << pat[i + 1...j].sub(/\A!/, "^") << "]"
          i = j
        else
          out << "\\["
        end
      when "{"
        depth += 1
        out << "(?:"
      when "}"
        if depth > 0
          depth -= 1
          out << ")"
        else
          out << "\\}"
        end
      when "," then out << (depth > 0 ? "|" : ",")
      else out << Regexp.escape(c)
      end
      i += 1
    end
    Regexp.new("\\A#{out}\\z")
  end

  # An Errno error with the system's text, as Ruby gives it ("No such file or directory - path").
  def __errno(klass, path) = SystemCallError.new(path.to_s, klass::Errno)

  # The entries of a folder; one that cannot be read is said on standard error and skipped, as
  # find(1) does, so a walk goes on (0.9.0 review).
  def __children(who, dir)
    Dir.children(dir).sort
  rescue SystemCallError => e
    $stderr.puts "rsh: #{who}: #{dir}: #{e.message.sub(/ - .*\z/m, "")}"
    []
  end

  # A path as the system resolves it (links followed where they exist).
  def __real(path)
    File.realpath(path)
  rescue SystemCallError
    File.expand_path(path)
  end

  def __seconds(v, what)
    return Time.now - v if v.is_a?(Time)
    return v.to_f if v.is_a?(Numeric)
    raise ArgumentError, "#{what}: give seconds (3600, 7 * 86400) or a Time"
  end

  # find(dir = ".", name: "*.log", type: :file|:dir|:link, older: s, newer: s, maxdepth: n,
  # hidden: true) -> Array of paths below dir, sorted, each "dir/..."; links are not followed.
  def find(dir = ".", name: nil, type: nil, older: nil, newer: nil, maxdepth: nil, hidden: true)
    raise ArgumentError, "find: type is :file, :dir or :link" unless [nil, :file, :dir, :link].include?(type)
    raise __errno(Errno::ENOENT, dir) unless File.directory?(dir.to_s)
    re = name && __name_re(name.to_s)
    old_s = older && __seconds(older, "older")
    new_s = newer && __seconds(newer, "newer")
    now = Time.now
    out = []
    walk = lambda do |d, depth|
      __children("find", d).each do |c|
        next if !hidden && c.start_with?(".")
        path = File.join(d, c)
        st = File.lstat(path)
        kind = st.symlink? ? :link : st.directory? ? :dir : st.file? ? :file : :other
        ok = (type.nil? || type == kind) && (re.nil? || c.match?(re))
        ok &&= now - st.mtime > old_s if old_s
        ok &&= now - st.mtime < new_s if new_s
        out << path if ok
        walk.call(path, depth + 1) if kind == :dir && (maxdepth.nil? || depth < maxdepth)
      end
    end
    walk.call(dir.to_s, 1)
    out
  end

  # grep(pattern, *files) -> the lines that match (a String pattern is plain text), each with
  # "file:" in front when more than one file is given.
  def grep(pattern, *files)
    re = pattern.is_a?(Regexp) ? pattern : Regexp.new(Regexp.escape(pattern.to_s))
    list = files.flatten.map(&:to_s)
    raise ArgumentError, "grep: give one file or more" if list.empty?
    list.flat_map do |f|
      File.readlines(f, chomp: true).select { |l| l.match?(re) }.map { |l| list.size > 1 ? "#{f}:#{l}" : l }
    end
  end

  # which(name) -> the full path of the program on PATH (empty parts skipped), or nil.
  def which(name)
    name = name.to_s
    return (File.file?(name) && File.executable?(name) ? name : nil) if name.include?("/")
    ENV.fetch("PATH", "/usr/local/bin:/usr/bin:/bin").split(":").each do |d|
      next if d.empty?
      p = File.join(d, name)
      return p if File.file?(p) && File.executable?(p)
    end
    nil
  end

  def exists?(path) = File.exist?(path.to_s)
  def file?(path) = File.file?(path.to_s)
  def dir?(path) = File.directory?(path.to_s)
  # Seconds since the file's contents last changed.
  def age(path) = Time.now - File.mtime(path.to_s)
  def size(path) = File.stat(path.to_s).size

  def read(path) = File.read(path.to_s)
  def write(path, text) = File.write(path.to_s, text.to_s)
  def append(path, text) = File.open(path.to_s, "a") { |f| f.write(text.to_s) }
  def lines(path) = File.readlines(path.to_s, chomp: true)

  # du(path) -> bytes in the files at and below path (links not followed).
  def du(path)
    st = File.lstat(path.to_s)
    return st.size unless st.directory?
    __children("du", path.to_s).sum { |c| du(File.join(path.to_s, c)) }
  end

  def mkdir_p(*dirs)
    dirs.flatten.each do |d|
      acc = d.to_s.start_with?("/") ? "" : nil
      d.to_s.split("/").each do |part|
        next if part.empty?
        acc = acc.nil? ? part : "#{acc}/#{part}"
        Dir.mkdir(acc) unless File.directory?(acc)
      end
    end
    nil
  end

  # rm(*files): removes files (not folders); a missing one is an error, as rm.
  def rm(*paths)
    paths.flatten.each { |p| File.delete(p.to_s) }
    nil
  end

  # rm_rf(*paths): files and folders; a link is removed, never followed; a missing path is fine.
  # It refuses "", "/", ".", "..", a path ending in /. or /.., and any path that is / or the current
  # folder once resolved (0.9.0 review): those are almost always a variable that was empty.
  def rm_rf(*paths)
    list = paths.flatten.map(&:to_s)
    list.each do |p|
      base = p.sub(%r{/+\z}, "")
      bad = p.empty? || base.empty? || %w[. ..].include?(File.basename(base)) || base == "."
      unless bad
        real = __real(p)
        bad = real == "/" || real == __real(".")
      end
      raise ArgumentError, "rm_rf: refusing to remove #{p.inspect} (it is /, the current folder, . or ..)" if bad
    end
    list.each { |p| __rm_tree(p) }
    nil
  end

  def __rm_tree(p)
    st = begin
      File.lstat(p)
    rescue SystemCallError
      return
    end
    if st.directory?
      Dir.children(p).each { |c| __rm_tree(File.join(p, c)) }
      Dir.rmdir(p)
    else
      File.delete(p)
    end
  end

  # cp(src, dst): one file (into dst when dst is a folder), with its mode. Returns the new path.
  def cp(src, dst)
    src = src.to_s
    dst = dst.to_s
    raise __errno(Errno::EISDIR, "#{src} (cp_r copies a folder)") if File.directory?(src)
    dst = File.join(dst, File.basename(src)) if File.directory?(dst)
    s = File.stat(src)
    if File.exist?(dst)
      d = File.stat(dst)
      raise ArgumentError, "cp: #{src} and #{dst} are the same file" if d.dev == s.dev && d.ino == s.ino
      dst = File.realpath(dst) if File.symlink?(dst)  # as cp: write where the link points
    end
    # into a new file next to dst, then renamed over it: dst is whole or untouched (0.9.0 review)
    tmp = File.join(File.dirname(dst), ".#{File.basename(dst)}.rsh-#{Process.pid}-#{rand(1 << 30).to_s(36)}")
    begin
      File.open(src, "rb") { |i| File.open(tmp, "wb") { |o| o.write(i.read) } }
      File.chmod(s.mode & 0o7777, tmp)
      File.rename(tmp, dst)
    rescue StandardError
      File.delete(tmp) if File.exist?(tmp)
      raise
    end
    dst
  end

  # cp_r(src, dst): a file or a whole folder (links copied as links).
  def cp_r(src, dst)
    src = src.to_s
    dst = dst.to_s
    dst = File.join(dst, File.basename(src)) if File.directory?(dst)
    st = File.lstat(src)
    if st.directory?
      from = __real(src)
      to = File.join(__real(File.dirname(dst)), File.basename(dst))
      raise ArgumentError, "cp_r: cannot copy #{src} into itself (#{dst})" if to == from || to.start_with?(from + "/")
    end
    if st.symlink?
      File.symlink(File.readlink(src), dst)
    elsif st.directory?
      Dir.mkdir(dst) unless File.directory?(dst)
      __children("cp_r", src).each { |c| cp_r(File.join(src, c), File.join(dst, c)) }
    else
      cp(src, dst)
    end
    dst
  end

  def mv(src, dst)
    dst = dst.to_s
    dst = File.join(dst, File.basename(src.to_s)) if File.directory?(dst)
    File.rename(src.to_s, dst)
    dst
  end

  def ln_s(target, link) = (File.symlink(target.to_s, link.to_s); link.to_s)

  def chmod(mode, *paths)
    paths.flatten.each { |p| File.chmod(mode, p.to_s) }
    nil
  end

  # touch(*files): makes each file, or sets its times to now.
  def touch(*paths)
    paths.flatten.each { |p| Rsh.__touch(p.to_s) }
    nil
  end

  # tmpdir(prefix = "rsh") { |dir| ... }: a new folder only you can read (in $TMPDIR, else /tmp),
  # removed afterwards, also when the block raises; without a block the folder stays.
  def tmpdir(prefix = "rsh")
    base = ENV["TMPDIR"].to_s.empty? ? "/tmp" : ENV["TMPDIR"]
    d = nil
    100.times do
      cand = File.join(base, "#{prefix}.#{Process.pid}.#{rand(1 << 30).to_s(36)}")
      begin
        Dir.mkdir(cand, 0o700)
        d = cand
        break
      rescue SystemCallError
        next
      end
    end
    raise Rsh::Error, "could not make a folder in #{base}" unless d
    return d unless block_given?
    begin
      yield d
    ensure
      rm_rf(d)
    end
  end
end

# The Ruby-standard names, for people who know them. Options (noop:, verbose:, force: ...) are not
# taken: a script that asks for one is told so instead of having it ignored.
module FileUtils
  extend self

  def self.respond_to_missing?(_name, _priv = false) = false

  def __plain(name, opts)
    raise ArgumentError, "FileUtils.#{name} takes no options in rsh (#{opts.keys.join(", ")})" unless opts.empty?
  end

  def mkdir_p(list, **o) = (__plain(:mkdir_p, o); Sh.mkdir_p(list); Array(list))
  def makedirs(list, **o) = mkdir_p(list, **o)
  def mkpath(list, **o) = mkdir_p(list, **o)
  def mkdir(list, **o) = (__plain(:mkdir, o); Array(list).each { |d| Dir.mkdir(d.to_s) }; Array(list))
  def rmdir(list, **o) = (__plain(:rmdir, o); Array(list).each { |d| Dir.rmdir(d.to_s) }; Array(list))
  def rm(list, **o) = (__plain(:rm, o); Sh.rm(list); Array(list))
  def rm_f(list, **o) = (__plain(:rm_f, o); Array(list).each { |f| File.delete(f.to_s) if File.exist?(f.to_s) || File.symlink?(f.to_s) }; Array(list))
  def rm_r(list, **o) = (__plain(:rm_r, o); Array(list).each { |f| raise Sh.__errno(Errno::ENOENT, f) unless File.exist?(f.to_s) || File.symlink?(f.to_s) }; Sh.rm_rf(list); Array(list))
  def rm_rf(list, **o) = (__plain(:rm_rf, o); Sh.rm_rf(list); Array(list))
  def rmtree(list, **o) = rm_rf(list, **o)
  def cp(src, dst, **o) = (__plain(:cp, o); Array(src).each { |s| Sh.cp(s, dst) }; nil)
  def cp_r(src, dst, **o) = (__plain(:cp_r, o); Array(src).each { |s| Sh.cp_r(s, dst) }; nil)
  def mv(src, dst, **o) = (__plain(:mv, o); Array(src).each { |s| Sh.mv(s, dst) }; 0)
  def ln_s(src, dst, **o) = (__plain(:ln_s, o); Sh.ln_s(src, dst); 0)
  def touch(list, **o) = (__plain(:touch, o); Sh.touch(list); Array(list))
  def chmod(mode, list, **o) = (__plain(:chmod, o); Sh.chmod(mode, list); Array(list))
  def pwd = Dir.pwd
end

# Find.find(*paths) { |path| ... }: every path at and below each one (the start too), in order;
# Find.prune in the block skips the folder it was given. Links are not followed.
module Find
  extend self

  def self.respond_to_missing?(_name, _priv = false) = false

  def find(*paths, &block)
    return to_enum(:find, *paths) unless block
    raise ArgumentError, "Find.find needs a path" if paths.empty?
    paths.each do |top|
      top = top.to_s
      raise Sh.__errno(Errno::ENOENT, top) unless File.exist?(top) || File.symlink?(top)
      __walk(top, &block)
    end
    nil
  end

  def __walk(path, &block)
    pruned = true
    catch(:__rsh_prune) do
      block.call(path.dup)
      pruned = false
    end
    return if pruned
    st = File.lstat(path)
    return unless st.directory?
    Sh.__children("find", path).each { |c| __walk(File.join(path, c), &block) }
  end

  def prune = throw(:__rsh_prune)
end

# Shellwords: text for another program's shell (ssh's remote command) and back. rsh itself never
# needs it: a value in a command line is one argument already.
module Shellwords
  extend self

  def self.respond_to_missing?(_name, _priv = false) = false

  def escape(str)
    s = str.to_s
    return "''" if s.empty?
    s.gsub(/[^A-Za-z0-9_\-.,:+\/@\n]/) { |c| "\\#{c}" }.gsub("\n", "'\n'")
  end

  def split(line)
    words = []
    field = +""
    have = false
    s = line.to_s
    i = 0
    while i < s.size
      c = s[i]
      if c == " " || c == "\t" || c == "\n"
        words << field if have
        field = +""
        have = false
      elsif c == "'"
        j = s.index("'", i + 1)
        raise ArgumentError, "Unmatched quote: #{line.inspect}" unless j
        field << s[i + 1...j]
        have = true
        i = j
      elsif c == '"'
        i += 1
        while i < s.size && s[i] != '"'
          if s[i] == "\\" && i + 1 < s.size && "$`\"\\\n".include?(s[i + 1])
            i += 1
            field << s[i]
          else
            field << s[i]
          end
          i += 1
        end
        raise ArgumentError, "Unmatched quote: #{line.inspect}" if i >= s.size
        have = true
      elsif c == "\\"
        # as Ruby's Shellwords: a backslash takes the next character, except a new line (both
        # stay); a backslash at the very end stays
        if i + 1 < s.size && s[i + 1] != "\n"
          i += 1
          field << s[i]
        elsif i + 1 < s.size
          i += 1
          field << "\\\n"
        else
          field << c
        end
        have = true
      else
        field << c
        have = true
      end
      i += 1
    end
    words << field if have
    words
  end

  def join(array) = array.map { |a| escape(a) }.join(" ")
  def shellescape(str) = escape(str)
  def shellsplit(line) = split(line)
  def shellwords(line) = split(line)
  def shelljoin(array) = join(array)
end

class String
  def shellescape = Shellwords.escape(self)
  def shellsplit = Shellwords.split(self)
end

class Array
  def shelljoin = Shellwords.join(self)
end

class Dir
  # Dir.mktmpdir(prefix = "d") { |dir| ... }: Sh.tmpdir, the Ruby-standard name.
  def self.mktmpdir(prefix = nil, &block) = Sh.tmpdir(prefix.nil? ? "d" : prefix.to_s, &block)
end
