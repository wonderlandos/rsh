# 0.1.4 (audit M-5): echo, pwd and type run in this process when alone, and a failed write is
# reported and is status 1 (it was lost in the forked child: exit 0, no message).
unless echo x > /dev/full
  puts "echo failed: #{$?.exitstatus}"
end
unless pwd > /dev/full
  puts "pwd failed: #{$?.exitstatus}"
end
unless type echo > /dev/full
  puts "type failed: #{$?.exitstatus}"
end
echo fine
puts(`echo captured`)
