# 0.1.4: reader fixes (audits R2-4, M-1, M-4, M-8, L-3).
# A Capital NAME=value with Ruby after it is Ruby (R2-4)
Point=Struct.new(:x, :y) do
  def sum = x + y
end
puts Point.new(1, 2).sum
Size=Data.define(:w, :h); s = Size.new(w: 3, h: 4); puts s.w * s.h
Limit=10; puts Limit
Total=Limit + 5 if true
puts Total
# ... and NAME=value command stays a command line
GREETING=hello sh -c 'echo $GREETING'
QUOTED="a b" sh -c 'echo "[$QUOTED]"'
# brace patterns as the first argument (M-1); a block stays a block
echo {a,b}
echo {x,y}.txt z
[1].each { |n| puts "block #{n}" }
# ! cmd: sh negation (M-4)
! test -e /no/such/file
puts "negated: #{$?.exitstatus}"
if ! test -e /no/such/file
  puts "if ! works"
end
n = 0
until ! test #{n} -lt 2
  n += 1
end
puts "until ! ended at #{n}"
# `end` after then/else/do on one line closes the Ruby construct (M-8)
if ARGV.empty? then echo no arguments end
if ARGV.empty? then echo first else echo second end
[1, 2].each do |k| echo item #{k} end
echo the end
# $$ is the process id (L-3)
pid = `echo $$`.chomp
puts pid == Process.pid.to_s
