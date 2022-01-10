// Copyright 2013-2018 Ayla Networks, Inc.  All rights reserved.

function banner(title)
{
	document.writeln("<title>" + title + "</title>" +
	    "</head><body>" +
	    "<center><div id=status></div></center>");
}

function elem_set(id, html)
{
	document.getElementById(id).innerHTML = html;
}

function escapeHtml(str) {
	var div = document.createElement('div');
	div.appendChild(document.createTextNode(str));
	return div.innerHTML;
}

function unescapeHtml(escapedStr) {
	var div = document.createElement('div');
	div.innerHTML = escapedStr;
	var child = div.childNodes[0];
	return child ? child.nodeValue : '';
}

function status_msg(msg, dismiss)
{
	var button = "Cancel";
	if (dismiss) {
		button = "Dismiss";
	}
	elem_set("status", "<div id=flash_alert>" + msg + "<p>" +
	    "<button onclick=\"conn_cancel()\">" + button + "</button></div>");
	document.getElementById("status").scrollIntoView(true);
}

function scan_td(item) {
	return "<td>" + item + "</td>";
}

function ssid_or_bssid(scan) {
	if (scan.bssid) {
		return "bssid=" + encodeURIComponent(scan.bssid);
	}
	return "ssid=" + encodeURIComponent(scan.ssid);
}

function wifi_spec_status_resp(response, status)
{
	var scan = window.scan_cur;
	var resp;
	var hist;
	var msg;
	var done = 0;

	if (status != 200) return;
	resp = JSON.parse(response).wifi_status;
	try {
		hist = resp.connect_history[0];
		if (hist.error != 20 && hist.last !== 0) {
			if (hist.error == 0) {
				msg = "Connection complete";
			} else {
				msg = "Connection failed: " +
					hist.msg + " (error " + hist.error + ")";
			}
			done = 1;
		} else {
			msg = "In progress";
			if (resp.state != null) {
				msg += ": " + resp.state;
			}
		}
	} catch(e) {
	}
	status_msg("Connection to " + escapeHtml(scan.ssid) +
	    "<br>" + msg, done);
	if (done == 1) clearInterval(window.sts_intvl);
	elem_set("div1", "");
	elem_set("div2", "");
}

function send_async_req(method, url, timeout, callback)
{
	var req = new XMLHttpRequest();
	var timedout = false;
	var timer = setTimeout(function() {
			timedout = true;
			req.abort();
		}, timeout);

	req.open(method, url, true);
	req.onreadystatechange = function() {
			if (req.readyState !== 4) return;
			if (timedout) return;
			clearTimeout(timer);
			callback(req.responseText, req.status);
	};
	req.send(null);
}

function send_sync_req(method, url, callback)
{
	var req = new XMLHttpRequest();
	req.open(method, url, false);
	req.send(null);
	callback(req.responseText, req.status);
}

function wifi_status_resp(response, status)
{
	window.wifi_status = JSON.parse(response).wifi_status;
}

function wifi_status_get(async)
{
	var url = "wifi_status.json";
	var callback = wifi_status_resp;
	if (async) {
		var scan = window.scan_cur;
		if (!scan) return;
		url += "?" + ssid_or_bssid(scan);
		callback = wifi_spec_status_resp;
		send_async_req("GET", url, 1000, callback);
		return;
	}
	send_sync_req("GET", url, callback);
}

function wifi_connect_resp(response, status)
{
	if (status >= 200 && status < 300) {
		window.sts_intvl = setInterval(wifi_status_get, 1000, 1);
		status_msg("Status unknown, please wait.<br>", 0);
	} else {
		var resp = JSON.parse(response);
		if (resp != null) {
			status_msg("Error: " + resp.msg, 0);
		} 
	}
}

function conn_start(key, scan)
{
	var url = "wifi_connect.json?" + ssid_or_bssid(scan);

	if (key != "") {
		url += "&key=" + encodeURIComponent(key);
	}
	send_async_req("POST", url, 1000, wifi_connect_resp);
}

function conn_ok()
{
	var scan = window.scan_cur;
	try {
		var ssid = document.getElementById("ssid");
		if (ssid) {
			scan.ssid = ssid.value;
		}
		var key = document.getElementById("key");
		if (key) {
			conn_start(key.value, scan);
		}
	} catch(e) {
		console.error("caught " + e.message);
	}
	return false;
}

function conn_cancel()
{
	window.scan_cur = null;
	elem_set("status", "");
	redraw();
}

function conn_prompt()
{
	var scan = window.scan_cur;
	var html = "<caption>Connect to network</caption>" +
	    "<tr><td>Network<td>";

	if (scan.ssid == "Join Other Network...") {
		html += "<input id=ssid size=15 autocomplete=off" +
		    "autofocus autocapitalize=off autocorrect=off" +
		    "required>";
	} else {
		html += escapeHtml(scan.ssid) +
		    "<tr><td>Security<td>" + scan.security;
	}
	html +=  "<tr><td>Password<td><input type=password id=key size=15 " +
	    "required autocapitalize=off autocorrect=off>" +
	    "<tr><td><td><button type=button " +
	    "onclick=\"conn_cancel()\">Cancel</button>" +
	    "&nbsp;&nbsp;&nbsp;<button>Connect</button>";
	elem_set("div1", "<form onSubmit=\"return conn_ok();\">" +
	    "<table class=networks>" + html + "</table></form>");
	elem_set("div2", "");
}

function connect(index)
{
	var scan = window.scan_results[index];

	window.scan_cur = scan;
	if (scan.security != "None") {
		conn_prompt();
	} else {
		conn_start("", scan);
	}
}

function scan_connect(index) {
	return scan_td("<button onclick=\"connect(" + index + ")\">" +
	    "Connect</button>");
}

function scan_line(index, scan) {
	var html = "<tr>";
	html += scan_td(escapeHtml(scan.ssid));

	html += "<td><table id=wifi_bars><tbody><tr>";
	for (var i = 0; i < scan.bars; i++) {
		html += "<td width=20px>&nbsp;";
	}
	html += "</tbody></table></td>";
	if (scan.type == "Ad hoc") {
		html += scan_td("ad-hoc");
	} else if (scan.type != "AP") {
		html += scan_td("-");
	} else {
		if (scan.ssid == window.wifi_status.connected_ssid) {
			html += scan_td("Connected");
		} else {
			html += scan_connect(index);
		}
		if (scan.security != "None" && scan.security != "Unknown") {
			html += scan_td("<img alt=Secure src=\"lock.gif\" " +
	    		    "style=\"height:0.92em;\">");
		}
	}
	html += "</tr>";
	return html;
}

function scan_tab() {
	var y = window.scan_results;

	var html = "<caption><h4>" +
	    "Select Wi-Fi Network<span id=refresh>" +
	    "<image src=refresh.gif alt=Refresh style=\"height:0.92em;\"" +
	    "onclick=\"rescan()\">" +
	    "</span>" +
	    "</h4></caption><thead><tr><th>Network</th><th>Strength</th>" +
	    "<th class=conn_action></th></tr></thead>" +
	    "<tbody>";
	for (var i = 0; i < y.length; i++) {
		html += scan_line(i, y[i]);
	}
	elem_set("div1", "<table class=networks>" + html + "</table>");
}

function wifi_scan_rslt_resp(response, status)
{
	var y;
	try {
		y = JSON.parse(response).wifi_scan.results;
		y.sort(function(a,b) {
			if (b.type != "AP") {
				return a.signal;
			}
			if (a.type != "AP") {
				return -b.signal;
			}
			if (a.signal == b.signal && a.ssid != b.ssid) {
				return b.ssid < a.ssid ? 1 : -1;
			}
			return b.signal - a.signal;
		});
	} catch(e) {
		y = new Array();
	}
	y.push({ssid:"Join Other Network...", bars:0, security:"Unknown", type:"AP"});
	window.scan_results = y;
	scan_tab();
}

function scan(response, status) {
	send_async_req("GET", "wifi_scan_results.json", 1000, wifi_scan_rslt_resp);
}

function rescan() {
	send_async_req("POST", "wifi_scan.json", 1000, scan);
}

function prof_del_resp(response, status)
{
	if (status < 299) {
		status_msg("Delete successful", 1);
	} else {
		status_msg("Delete failed", 1);
	}
	update();
}

function del_ok(esc_ssid)
{
	var ssid = unescapeHtml(esc_ssid);
	var url = "wifi_profile.json?ssid=" +
		encodeURIComponent(ssid);

	send_async_req("DELETE", url, 1000, prof_del_resp);
}

function prof_delete(esc_ssid) {
	var xml = "<table><caption>Confirm ";
	var ssid = unescapeHtml(esc_ssid);

	if (ssid == window.wifi_status.connected_ssid) {
		xml += "disconnect and ";
	}
	xml += "delete of network " + esc_ssid + "</caption><tr><td>" +
	    "<button type=button onclick=\"redraw()\">Cancel</button>" +
	    "<td><button type=button onclick=\"del_ok(&quot;" +
	    esc_ssid +
	    "&quot;)\">OK</button></table>";
	elem_set("div1", xml);
	elem_set("div2", "");
}

function prof_line(prof) {
	var button;

	html = "<tr><td>" + escapeHtml(prof.ssid) + "<td>";
	if (prof.ssid == window.wifi_status.connected_ssid) {
		html += "Connected";
		button = "Disconnect";
	} else {
		button = "Delete";
	}
	return html + "<td><button onclick=\"prof_delete(&quot;" +
	     escapeHtml(prof.ssid) + "&quot;)\">" + button + "</button>";
}

function prof_tab(x) {
	var y = window.proftab;
	var html;

	html = "<table class=networks>" + 
	    "<caption><h4>Wi-Fi Profiles</h4></caption><thead>" +
	    "<tr><th>Network<th>Status<th><th><tbody>";

	if (y != null) {
		y.sort(function(a,b) {
			if (b.ssid == a.ssid) {
				return 0;
			}
			return b.ssid < a.ssid ? 1 : -1;
		});
		for (var i = 0; i < y.length; i++) {
			html += prof_line(y[i]);
		}
	}
	html += "</table>"
	elem_set("div2", html);
}

function wifi_prof_resp(response, status)
{
	window.proftab = JSON.parse(response).wifi_profiles;
	prof_tab();
}

function profiles() {
	send_async_req("GET", "wifi_profiles.json", 1000, wifi_prof_resp);
}

function redraw() {
	scan_tab();
	prof_tab();
}

function update() {
	profiles();
	scan();
}
wifi_status_get(0);
banner(window.wifi_status.host_symname + " " + "Wifi Status");
document.writeln("<section><div class=centered>" +
    "<br><div id=div1></div><br><br><div id=div2></div>");
update();
