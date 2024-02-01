require 'mkmf'

$INCFLAGS << ' -I$(srcdir)/vendor/include'
CONFIG['debugflags'] << ' -ggdb3 -O0'
#CONFIG['debugflags'] << ' -ggdb3'

create_makefile('core')
