// FruityMulti - relais des sessions.
//
// Un salon par code de session : l'hote l'ouvre, l'invite le rejoint, et le
// relais recopie chaque message de l'un vers l'autre sans le lire. Personne
// n'ouvre de port, personne ne voit l'IP de l'autre.
//
// Tourne sur Cloudflare Workers (offre gratuite) : un Durable Object par salon,
// avec l'API d'hibernation pour que le salon ne coute rien quand il se tait.
//
//   wss://<relais>/salon/<CODE>?role=hote|invite&proto=<n>
//
// Messages du relais lui-meme (jamais produits par un agent) :
//   {"_relais":"pair_arrive"}   les deux places sont prises
//   {"_relais":"pair_parti"}    l'autre a ferme
//   {"_relais":"refuse","raison":"..."} puis fermeture

import { DurableObject } from "cloudflare:workers";

const PROTO = 1;
const CODE = /^[A-HJ-NP-Z2-9]{6}$/;   // sans I, O, 0, 1 : impossibles a confondre
const TAILLE_MAX = 1024 * 1024;

export default {
	async fetch (requete, env)
	{
		const url = new URL (requete.url);

		if (url.pathname === "/" || url.pathname === "/sante")
			return new Response (`FruityMulti relais, protocole ${PROTO}\n`);

		const morceaux = url.pathname.split ("/").filter (Boolean);
		if (morceaux.length !== 2 || morceaux[0] !== "salon")
			return new Response ("introuvable\n", { status: 404 });

		const code = morceaux[1].toUpperCase ();
		if (!CODE.test (code))
			return new Response ("code invalide\n", { status: 400 });

		if (requete.headers.get ("Upgrade") !== "websocket")
			return new Response ("WebSocket attendu\n", { status: 426 });

		const salon = env.SALONS.get (env.SALONS.idFromName (code));
		return salon.fetch (requete);
	},
};

export class Salon extends DurableObject
{
	async fetch (requete)
	{
		const url = new URL (requete.url);
		const role = url.searchParams.get ("role");
		const proto = Number (url.searchParams.get ("proto"));

		const [client, serveur] = Object.values (new WebSocketPair ());

		const raison = this.refus (role, proto);
		if (raison !== null)
		{
			// on accepte pour pouvoir expliquer pourquoi, puis on ferme
			serveur.accept ();
			serveur.send (JSON.stringify ({ _relais: "refuse", raison }));
			serveur.close (4000, "refuse");
			return new Response (null, { status: 101, webSocket: client });
		}

		// meme role deja present : c'est une reconnexion, l'ancienne place
		// est morte (coupure reseau, agent relance)
		for (const ancien of this.ctx.getWebSockets (role))
		{
			try { ancien.close (4001, "remplace"); } catch (e) { }
		}

		this.ctx.acceptWebSocket (serveur, [role]);

		if (this.present ("hote") && this.present ("invite"))
			this.diffuser ({ _relais: "pair_arrive" });

		return new Response (null, { status: 101, webSocket: client });
	}

	refus (role, proto)
	{
		if (role !== "hote" && role !== "invite")
			return "role inconnu";
		if (proto !== PROTO)
			return `protocole ${proto} ici, ${PROTO} sur le relais : mets FruityMulti a jour`;
		if (role === "invite" && !this.present ("hote"))
			return "aucune session ouverte avec ce code";
		return null;
	}

	present (role)
	{
		return this.ctx.getWebSockets (role).length > 0;
	}

	diffuser (message)
	{
		const texte = JSON.stringify (message);
		for (const ws of this.ctx.getWebSockets ())
		{
			try { ws.send (texte); } catch (e) { }
		}
	}

	autre (ws)
	{
		const [role] = this.ctx.getTags (ws);
		return this.ctx.getWebSockets (role === "hote" ? "invite" : "hote");
	}

	async webSocketMessage (ws, message)
	{
		const taille = typeof message === "string" ? message.length : message.byteLength;
		if (taille > TAILLE_MAX)
			return;
		for (const pair of this.autre (ws))
		{
			try { pair.send (message); } catch (e) { }
		}
	}

	async webSocketClose (ws)
	{
		for (const pair of this.autre (ws))
		{
			try { pair.send (JSON.stringify ({ _relais: "pair_parti" })); } catch (e) { }
		}
		// l'hote parti, le salon n'a plus de raison d'exister
		const [role] = this.ctx.getTags (ws);
		if (role === "hote")
		{
			for (const pair of this.autre (ws))
			{
				try { pair.close (4002, "hote parti"); } catch (e) { }
			}
		}
	}

	async webSocketError (ws)
	{
		await this.webSocketClose (ws);
	}
}
