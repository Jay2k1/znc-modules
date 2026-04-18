# znc-modules
My modules(s) for the ZNC IRC bouncer

## monitor_multiclient.cpp
This module improves MONITOR usage when using multiple clients.

It mainly solves two problems:

1. A client attaches to ZNC and opens a few query windows. This
client shows online/offline/away indicators on the queries using
MONITOR, so upon opening the queries, it sends `MONITOR + nick`
for these micks, gets the responses and displays the indicators
accordingly.
Now the user quits the client and re-opens it. The client restores
the previously opened query windows and again issues `MONITOR + nick`
for them. Libera for example won't send any response to that, because
the nick is already on the server's MONITOR list for that connection,
as ZNC kept the connection alive.

2. Two clients are attached to ZNC, both open a query window with nick,
then client 1 closes that query window and hence issues `MONITOR - nick`.
Client 2 still has that query window open but the status indicator will
no longer be updated, because nick is no longer on the MONITOR list,
unbeknownst to client 2. It is even worse if client 1 issues `MONITOR C`.

