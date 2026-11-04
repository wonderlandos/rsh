# 0.1.4 (audit R2-7): after && or || a Ruby control word ends the command line; the rest is Ruby.
def check(dir)
  ls #{dir} >/dev/null 2>&1 || return "missing #{dir}"
  "found #{dir}"
end
puts check("/")
puts check("/no/such/dir")
begin
  sh -c "exit 3" || raise "it failed with #{$?.exitstatus}"
rescue RuntimeError => e
  puts e.message
end
sh -c "exit 1" || :
puts "after || : the status is #{$?.exitstatus}"
[1, 2, 3].each do |n|
  test #{n} -eq 2 && next
  puts "item #{n}"
end
x = 0
loop do
  x += 1
  test #{x} -lt 3 || break
end
puts "loop ended at #{x}"
begin
  ls /no/such/dir 2>/dev/null ||
    raise("the Ruby may go on the next line")
rescue RuntimeError => e
  puts e.message
end
ls / >/dev/null && exit 7
puts "not reached"
