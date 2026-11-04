# SPDX-License-Identifier: GPL-2.0-or-later
# Bare words the reader cannot know (methods made at run time by method_missing) need one of:
# rsh_methods :name (by name), `ruby do ... end` / `ruby { ... }` (a block), `ruby!` (the rest).
class Recorder
  attr_reader :calls

  def initialize = (@calls = [])

  def method_missing(name, *args)
    @calls << [name, *args]
    self
  end

  def respond_to_missing?(*) = true
end

r = Recorder.new
r.instance_eval do
  ruby do
    build "app"
    if calls.size == 1
      copy "dist", to: "/srv"
    end
    [1, 2].each do |i|
      step i
    end
  end
  echo commands again after the block
end
p r.calls

ruby { r.instance_eval { finish } }
p r.calls.last
echo #{r.calls.size} calls

ruby!
r.instance_eval { echo "now echo is the Recorder's" }
p r.calls.last
$ echo a leading dollar still runs a program
