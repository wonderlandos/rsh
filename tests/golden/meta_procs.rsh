# SPDX-License-Identifier: GPL-2.0-or-later
# Proc, lambda, currying, Method objects, Struct with Comparable, Data, and what the built-in
# interpreter does not have (refinements).
add = ->(a, b, c) { a + b + c }
p add.curry[1][2][3], add.curry.(1).(2, 3), add.arity, add.lambda?
loose = proc { |a, b| [a, b] }
p loose.call(1), loose.call(1, 2, 3), loose.arity, loose.lambda?
twice = :upcase.to_proc >> ->(s) { s * 2 }
p twice.call("ab"), %w[a b].map(&:upcase)
run = ->(*words) { echo #{words} }
run.call("x y", "z")
m = 12.method(:+)
p m.call(30), [1, 2].map(&10.method(:*)), m.owner, m.unbind.class

counter = Enumerator.new { |y| 3.times { |i| y << i * i } }
p counter.to_a, counter.lazy.map { |x| x + 1 }.select(&:odd?).first(2)

Version = Struct.new(:major, :minor) do
  include Comparable

  def <=>(other)
    to_a <=> other.to_a
  end

  def to_s = "#{major}.#{minor}"
end
vs = [Version.new(1, 10), Version.new(1, 2), Version.new(0, 9)]
puts vs.sort.join(" "), vs.max, Version.new(1, 2).between?(vs[2], vs[0]), vs[1].clamp(vs[2], vs[2])
p Version.members, Version.new(3, 4).to_h

Point = Data.define(:x, :y) do
  def norm = x.abs + y.abs
end
pt = Point.new(x: 3, y: -4)
p pt.norm, pt.to_h, pt.with(y: 0), pt == Point.new(x: 3, y: -4)

p Module.private_instance_methods.include?(:refine), respond_to?(:using, true)
