# SPDX-License-Identifier: GPL-2.0-or-later
# define_method with a literal name, attr_reader and Struct members: the reader knows these bare
# words are Ruby. Names made from a list are only known at run time: rsh_methods declares them.
class Site
  attr_reader :name, :port
  define_method(:banner) { "site #{name}:#{port}" }

  def initialize(name, port)
    @name = name
    @port = port
  end

  def show
    banner
  end

  %w[start stop].each do |verb|
    define_method("#{verb}!") { echo #{verb} #{name} }
    define_method(verb) { "#{verb} #{name}" }
  end
  rsh_methods :start, :stop

  def cycle
    a = stop
    b = start
    "#{a}, then #{b}"
  end
end

s = Site.new("a b; rm -rf /", 8080)
puts s.show
puts s.cycle
s.start!
s.stop!
p Site.instance_methods(false).sort
p s.respond_to?(:start), s.method(:banner).arity

Point = Struct.new(:px, :py) do
  def sum
    px + py
  end

  def to_s
    "(#{px}, #{py})"
  end
end
puts Point.new(2, 3).sum, Point.new(1, 2)

# Without the declaration a bare word is a command, as the rule says: `missing_tool` is not a
# method the reader can see, so it is run (and fails, here caught).
begin
  missing_tool_zz 2>/dev/null
rescue Rsh::Failed => e
  puts "command: #{e.message}"
end
