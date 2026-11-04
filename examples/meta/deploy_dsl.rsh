#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# deploy_dsl: a deploy written in a small language made with method_missing.
# Usage: deploy_dsl.rsh [--dry-run] [VERSION]
#
#   app "shop" do
#     hosts "web1", "web2"
#     git.fetch "--tags"            # <program>.<subcommand> words...: any program, no list to keep
#     git.checkout "v#{version}"
#     make "-j4"                    # <program> words...: declared with rsh_methods below
#     systemctl.restart "shop"
#   end
#
# Every word of a step is one argument (method_missing collects them as Ruby values; nothing is
# split or expanded). Locally a step runs as `$ #{argv}`; for a remote host each word is quoted for
# the remote shell and the line goes to ssh as one argument. A host that fails is reported and
# the others go on.

# The DSL words used without a receiver: the reader must know they are Ruby, not programs.
rsh_methods :hosts, :make, :rsync, :echo_step

class Program
  def initialize(plan, words)
    @plan = plan
    @words = words
  end

  # git.fetch "--tags" -> step ["git", "fetch", "--tags"]
  def method_missing(sub, *args)
    @plan.add(@words + [sub.to_s.tr("_", "-"), *args.map(&:to_s)])
  end

  def respond_to_missing?(*) = true
end

class Plan
  attr_reader :name, :version, :steps

  def initialize(name, version)
    @name = name
    @version = version
    @hosts = ["local"]
    @steps = []
  end

  def hosts(*list) = (@hosts = list.flatten)
  def add(argv) = (@steps << argv; self)

  # `git`, `systemctl`, ...: a Program that turns the next call into a step.
  def method_missing(prog, *args)
    return super unless prog.to_s.match?(/\A[a-z][a-z0-9_]*\z/)
    args.empty? ? Program.new(self, [prog.to_s]) : add([prog.to_s, *args.map(&:to_s)])
  end

  def respond_to_missing?(prog, priv = false) = prog.to_s.match?(/\A[a-z][a-z0-9_]*\z/) || super

  # Words named with rsh_methods come here directly too (they are methods of the plan).
  %w[make rsync].each { |prog| define_method(prog) { |*args| add([prog, *args.map(&:to_s)]) } }
  def echo_step(*words) = add(["echo", *words])

  def quote(word) = "'" + word.gsub("'") { %q('\\'') } + "'"

  def run(dry)
    failed = []
    @hosts.each do |host|
      puts "#{name} #{version} on #{host}"
      @steps.each do |argv|
        if dry
          puts "  would run: #{argv.inspect}"
        elsif host == "local"
          $ #{argv}
        else
          ssh -o BatchMode=yes #{host} #{argv.map { |w| quote(w) }.join(" ")}
        end
      end
    rescue Rsh::Failed => e
      puts "#{host}: failed (#{e.status.exitstatus}) #{e.stderr.strip}"
      failed << host
    end
    failed
  end
end

def app(name, &block)
  dry = ARGV.delete("--dry-run")
  plan = Plan.new(name, ARGV[0] || "1.0")
  plan.instance_eval(&block)
  failed = plan.run(dry)
  puts failed.empty? ? "deployed #{name} #{plan.version}" : "failed on: #{failed.join(', ')}"
  failed.empty?
end

ok = app "shop" do
  hosts "local", "web1", "down", "web2"
  git.fetch "--tags"
  git.checkout "v#{version}"
  make "-j4", "VERSION=#{version}"
  echo_step "release; it's #{version}", "$HOME", "*"
  systemctl.restart "shop"
end
exit(ok ? 0 : 1)
