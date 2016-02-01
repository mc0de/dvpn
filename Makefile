all:		dvpn rtmon topomon topowalk

clean:
		rm -f *.dot
		rm -f client.ini
		rm -f client.key
		rm -f dvpn
		rm -f rtmon
		rm -f server.ini
		rm -f server.key
		rm -f topomon
		rm -f topowalk

install:	dvpn
		install -m 0755 dvpn /usr/bin
		install -m 0644 dvpn.service /usr/lib/systemd/system

dvpn:		dvpn.c adj_rib_in.c adj_rib_in.h conf.c conf.h confdiff.c confdiff.h cspf.c cspf.h dgp_connect.c dgp_connect.h dgp_listen.c dgp_listen.h dgp_reader.c dgp_reader.h dgp_writer.c dgp_writer.h itf.c itf.h iv_getaddrinfo.c iv_getaddrinfo.h loc_rib.c loc_rib.h loc_rib_print.c loc_rib_print.h lsa.c lsa.h lsa_deserialise.c lsa_deserialise.h lsa_diff.c lsa_diff.h lsa_path.c lsa_path.h lsa_print.c lsa_print.h lsa_serialise.c lsa_serialise.h lsa_type.h rib_listener.h rib_listener_to_loc.c rib_listener_to_loc.h rt_builder.c rt_builder.h spf.c spf.h tconn.c tconn.h tconn_connect.c tconn_connect.h tconn_listen.c tconn_listen.h tun.c tun.h util.c util.h x509.c x509.h
		gcc -Wall -g -o dvpn dvpn.c adj_rib_in.c conf.c confdiff.c cspf.c dgp_connect.c dgp_listen.c dgp_reader.c dgp_writer.c itf.c iv_getaddrinfo.c loc_rib.c loc_rib_print.c lsa.c lsa_deserialise.c lsa_diff.c lsa_path.c lsa_print.c lsa_serialise.c rib_listener_to_loc.c rt_builder.c spf.c tconn.c tconn_connect.c tconn_listen.c tun.c util.c x509.c -lgnutls -lini_config -livykis -lnettle

rtmon:		rtmon.c adj_rib_in.c adj_rib_in.h conf.c conf.h cspf.c cspf.h dgp_connect.c dgp_connect.h dgp_reader.c dgp_reader.h dgp_writer.c dgp_writer.h loc_rib.c loc_rib.h loc_rib_print.c loc_rib_print.h lsa.c lsa.h lsa_deserialise.c lsa_deserialise.h lsa_diff.c lsa_diff.h lsa_path.c lsa_path.h lsa_print.c lsa_print.h lsa_serialise.c lsa_serialise.h rib_listener.h rib_listener_debug.c rib_listener_debug.h rib_listener_to_loc.c rib_listener_to_loc.h rt_builder.c rt_builder.h spf.c spf.h util.c util.h x509.c x509.h
		gcc -Wall -g -o rtmon rtmon.c adj_rib_in.c conf.c cspf.c dgp_connect.c dgp_reader.c dgp_writer.c loc_rib.c loc_rib_print.c lsa.c lsa_deserialise.c lsa_diff.c lsa_path.c lsa_print.c lsa_serialise.c rib_listener_debug.c rib_listener_to_loc.c rt_builder.c spf.c util.c x509.c -lgnutls -lini_config -livykis -lnettle

topomon:	topomon.c adj_rib_in.c adj_rib_in.h conf.c conf.h dgp_connect.c dgp_connect.h dgp_reader.c dgp_reader.h dgp_writer.c dgp_writer.h loc_rib.c loc_rib.h loc_rib_print.c loc_rib_print.h lsa.c lsa.h lsa_deserialise.c lsa_deserialise.h lsa_diff.c lsa_diff.h lsa_path.c lsa_path.h lsa_print.c lsa_print.h lsa_serialise.c lsa_serialise.h rib_listener.h rib_listener_debug.c rib_listener_debug.h rib_listener_to_loc.c rib_listener_to_loc.h util.c util.h x509.c x509.h
		gcc -Wall -g -o topomon topomon.c adj_rib_in.c conf.c dgp_connect.c dgp_reader.c dgp_writer.c loc_rib.c loc_rib_print.c lsa.c lsa_deserialise.c lsa_diff.c lsa_path.c lsa_print.c lsa_serialise.c rib_listener_debug.c rib_listener_to_loc.c util.c x509.c -lgnutls -lini_config -livykis -lnettle

topowalk:	topowalk.c conf.c conf.h cspf.c cspf.h lsa.c lsa.h lsa_deserialise.c lsa_deserialise.h lsa_type.h spf.c spf.h util.c util.h x509.c x509.h
		gcc -Wall -g -o topowalk topowalk.c conf.c cspf.c lsa.c lsa_deserialise.c spf.c util.c x509.c -lgnutls -lini_config -livykis -lnettle

test:		client.ini client.key dvpn server.ini server.key

client.ini:	server.key dvpn
		@echo PrivateKey=client.key > client.ini
		@echo NodeName=client >> client.ini
		@echo >> client.ini
		@echo [local] >> client.ini
		@echo Connect=localhost:19275 >> client.ini
		@echo PeerFingerprint=`./dvpn --show-key-id server.key` >> client.ini
		@echo PeerType=peer >> client.ini
		@echo TunInterface=tapc%d >> client.ini

client.key:
		certtool --generate-privkey --rsa --sec-param=high --outfile client.key

server.ini:	client.key dvpn
		@echo PrivateKey=server.key > server.ini
		@echo NodeName=server >> server.ini
		@echo >> server.ini
		@echo [local] >> server.ini
		@echo Listen=0.0.0.0:19275 >> server.ini
		@echo PeerFingerprint=`./dvpn --show-key-id client.key` >> server.ini
		@echo PeerType=peer >> server.ini
		@echo TunInterface=tap0 >> server.ini

server.key:
		certtool --generate-privkey --rsa --sec-param=high --outfile server.key
