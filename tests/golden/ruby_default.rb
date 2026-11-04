# A .rb file has the shell layer, as a .rsh file does: Ruby and command lines mix the same way.
def twice(x) = x * 2
dir = "with space"
mkdir -p #{dir}
echo #{twice(21)} > "#{dir}/n.txt"
puts File.read("#{dir}/n.txt").strip.to_i + 1
ls | { |l| l.upcase }
nosuchcommand_zz || echo not a Ruby method, a command
