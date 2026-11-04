#!/usr/bin/rsh
# SPDX-License-Identifier: GPL-2.0-or-later
# tasks: a small task runner. A task is a method made with define_method; dependencies run first
# and once. Usage: tasks.rsh [--list] [TASK...]   (no task: the task named `default`)
#
#   desc "compile"
#   task build: :clean do     # a task with a dependency
#     make all                # command lines and Ruby, as anywhere in rsh
#   end

class Runner
  @tasks = {}

  class << self
    # `def` makes desc and task known words of the file: in a class body they are Ruby.
    def desc(text) = (@desc = text)

    def task(spec, &body)
      name, deps = spec.is_a?(Hash) ? spec.first : [spec, []]
      Runner.tasks[name.to_sym] = {deps: Array(deps).map(&:to_sym), desc: @desc}
      @desc = nil
      define_method("task_#{name}", &body)
    end

    def tasks = @tasks || Runner.instance_variable_get(:@tasks)
  end

  def initialize = (@done = [])

  def invoke(name, chain = [])
    name = name.to_sym
    raise ArgumentError, "no task #{name} (tasks.rsh --list)" unless respond_to?("task_#{name}")
    raise ArgumentError, "a cycle: #{(chain + [name]).join(' -> ')}" if chain.include?(name)
    return if @done.include?(name)
    Runner.tasks[name][:deps].each { |d| invoke(d, chain + [name]) }
    puts "== #{name}"
    public_send("task_#{name}")
    @done << name
  end

  def self.list
    Runner.tasks.sort.each do |name, t|
      deps = t[:deps].empty? ? "" : " (after #{t[:deps].join(', ')})"
      puts format("%-8s %s%s", name, t[:desc], deps)
    end
  end
end

class Project < Runner
  attr_reader :version

  def initialize
    super
    @version = "2.1 rc"
  end

  desc "remove the build directory"
  task :clean do
    rm -rf build
  end

  desc "compile into build/"
  task build: :clean do
    mkdir -p build
    make all VERSION=#{version} > build/make.log
    cat build/make.log
  end

  desc "count what was built"
  task check: :build do
    n = Dir.glob("build/*").size
    puts "#{n} file(s) in build"
  end

  desc "build and check"
  task default: [:build, :check] do
    echo done #{version}
  end
end

if ARGV.include?("--list")
  Runner.list
  exit 0
end
runner = Project.new
names = ARGV.empty? ? [:default] : ARGV
begin
  names.each { |t| runner.invoke(t) }
rescue ArgumentError => e
  warn "tasks: #{e.message}"
  exit 2
end
