dist_man8_MANS=lighttpd.8

DOCS=accesslog.txt \
authentication.txt \
cgi.txt \
compress.txt \
configuration.txt \
features.txt \
performance.txt \
plugins.txt \
redirect.txt \
rewrite.txt \
secdownload.txt \
security.txt \
simple-vhost.txt \
skeleton.txt \
ssi.txt \
ssl.txt \
state.txt \
rrdtool.txt \
alias.txt \
userdir.txt \
access.txt \
traffic-shaping.txt \
setenv.txt \
status.txt \
trigger_b4_dl.txt \
webdav.txt \
expire.txt \
dirlisting.txt \
evhost.txt

HTMLDOCS=accesslog.html \
	 authentication.html \
	 cgi.html \
	 compress.html \
	 configuration.html \
	 features.html \
	 performance.html \
	 plugins.html \
	 proxy.html \
	 redirect.html \
	 rewrite.html \
	 secdownload.html \
	 security.html \
	 simple-vhost.html \
	 skeleton.html \
	 ssi.html \
	 ssl.html \
	 state.html \
	 rrdtool.html \
	 alias.html \
	 userdir.html \
	 access.html \
	 traffic-shaping.html \
	 setenv.html \
	 status.html \
	 trigger_b4_dl.html \
	 webdav.html \
	 expire.html \
	 dirlisting.html \
	 evhost.html

EXTRA_DIST=lighttpd.conf lighttpd.user \
	rc.lighttpd rc.lighttpd.redhat sysconfig.lighttpd \
	rrdtool-graph.sh \
	state.dot \
	spawn-php.sh \
	newstyle.css \
	oldstyle.css \
	$(DOCS)

.txt.html:
	rst2html $^ > $@


html: $(HTMLDOCS)

#%.ps.gz: %.ps
#	gzip $^
	
#%.ps: %.dot
#	dot -Tps -o $@ $^

clean-local:
	rm -f *.html
