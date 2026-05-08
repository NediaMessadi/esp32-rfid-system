const mqtt = require('mqtt');
const mysql = require('mysql2');

function normalizeButton(value) {
  if (!value) return null;
  const v = String(value).trim().toLowerCase();
  if (v === 'btn1' || v === '1' || v === 'button1') return 'btn1';
  if (v === 'btn2' || v === '2' || v === 'button2') return 'btn2';
  return null;
}

function normalizeUid(value) {
  if (!value) return '';
  return String(value)
    .toUpperCase()
    .replace(/[^A-F0-9]/g, '');
}

function parseAssignedTime(startTime) {
  if (!startTime) return null;
  const str = String(startTime).trim();
  const parts = str.split(':').map(Number);
  if (parts.length < 2 || Number.isNaN(parts[0]) || Number.isNaN(parts[1])) return null;
  const h = parts[0];
  const m = parts[1];
  const s = parts.length > 2 && !Number.isNaN(parts[2]) ? parts[2] : 0;
  return { h, m, s, asText: `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}` };
}

function computeStatus(assigned) {
  if (!assigned) return 'ok';
  const now = new Date();
  const limit = new Date();
  limit.setHours(assigned.h, assigned.m, assigned.s, 0);
  return now > limit ? 'late' : 'ok';
}

// 🔗 Connexion MQTT
const client = mqtt.connect('mqtt://localhost');

// 🔗 Connexion MySQL
const db = mysql.createConnection({
  host: 'localhost',
  user: 'root',
  password: '',
  database: 'cm2e'
});

// 📡 MQTT connecté
// Dans client.on('connect', ...) — publier un retained vide pour effacer l'ancien
client.on('connect', () => {
  console.log("MQTT connecté");
  // ✅ Effacer tout message retained sur decision
  client.publish('pointage/decision', '', { retain: true });
  client.subscribe('pointage/action');
});

// 📥 Réception UID
// Requête corrigée : bouton assigné vient de working_time_assignments ou users
// Si vous n'avez pas de colonne button dans users, ajoutez-la :
// ALTER TABLE users ADD COLUMN button_assigned VARCHAR(10) DEFAULT NULL;

client.on('message', (topic, message) => {
  const raw = message.toString().trim();
  const parts = raw.split('|');

  const uid    = normalizeUid(parts[0]);
  const button = normalizeButton(parts[1]);

  console.log(`[SCAN] uid=${uid} button=${button}`);

  if (!uid || !button) {
    console.log('[SCAN] Payload invalide');
    client.publish('pointage/decision', `${uid || 'UNKNOWN'}|Invalide|no`);
    return;
  }

  // ✅ DB query DANS le handler — pas en dehors
  db.query(
    `SELECT id, prenom, nom, button_assigned
     FROM users
     WHERE UPPER(REPLACE(REPLACE(REPLACE(badge_uid, ' ', ''), ':', ''), '-', '')) = ?
       AND active = 1`,
    [uid],
    (err, results) => {
      if (err || results.length === 0) {
        console.log('[DB] Badge inconnu:', uid);
        client.publish('pointage/decision', `${uid}|Inconnu|no`);
        return;
      }

      const user = results[0];
      const name = `${user.prenom} ${user.nom}`;

      const assignedButton = normalizeButton(user.button_assigned);
      if (!assignedButton || assignedButton !== button) {
        console.log(`[SCAN] Mauvais bouton: attendu=${assignedButton} reçu=${button}`);
        client.publish('pointage/decision', `${uid}|${name}|no`);
        return;
      }

      db.query(
        `SELECT id, arrivee, depart, status, assigned_time
         FROM pointages
         WHERE user_id = ? AND date = CURDATE()
         ORDER BY id DESC LIMIT 1`,
        [user.id],
        (errPt, rows) => {
          if (errPt) {
            console.error('[DB] Erreur pointage:', errPt);
            client.publish('pointage/decision', `${uid}|${name}|no`);
            return;
          }

          const lastRow = rows.length > 0 ? rows[0] : null;
          const hasOpenSession = !!lastRow && !lastRow.depart;

          if (hasOpenSession && !lastRow.arrivee) {
            const assigned = parseAssignedTime(lastRow.assigned_time);
            const status = computeStatus(assigned);

            db.query(
              `UPDATE pointages
               SET arrivee = NOW(), status = ?, badge_uid = ?,
                   rfid_scan = 1, created_at = COALESCE(created_at, NOW())
               WHERE id = ?`,
              [status, uid, lastRow.id],
              (errUpdStart) => {
                if (errUpdStart) {
                  console.error('[DB] UPDATE arrivée erreur:', errUpdStart);
                  client.publish('pointage/decision', `${uid}|${name}|no`);
                  return;
                }
                console.log(`[DB] Arrivée enregistrée — ${name} (${status})`);
                client.publish('pointage/decision', `${uid}|${name}|${status}`);
              }
            );
            return;
          }

          if (hasOpenSession && lastRow.arrivee) {
            db.query(
              `UPDATE pointages
               SET depart = NOW(),
                   duree = TIME_FORMAT(TIMEDIFF(NOW(), arrivee), '%H:%i'),
                   badge_uid = COALESCE(badge_uid, ?), rfid_scan = 1
               WHERE id = ?`,
              [uid, lastRow.id],
              (errUpdEnd) => {
                if (errUpdEnd) {
                  console.error('[DB] UPDATE départ erreur:', errUpdEnd);
                  client.publish('pointage/decision', `${uid}|${name}|no`);
                  return;
                }
                const statusToSend = lastRow.status || 'ok';
                console.log(`[DB] Départ enregistré — ${name}`);
                client.publish('pointage/decision', `${uid}|${name}|${statusToSend}`);
              }
            );
            return;
          }

          db.query(
            `SELECT start_time FROM working_time_assignments
             WHERE user_id = ? AND work_date = CURDATE() LIMIT 1`,
            [user.id],
            (errSched, schedule) => {
              const assigned = (!errSched && schedule.length > 0)
                ? parseAssignedTime(schedule[0].start_time) : null;
              const status = computeStatus(assigned);
              const assignedTimeForDb = assigned ? assigned.asText : null;

              db.query(
                `INSERT INTO pointages
                   (user_id, date, arrivee, status, badge_uid, rfid_scan, created_at, assigned_time)
                 VALUES (?, CURDATE(), NOW(), ?, ?, 1, NOW(), ?)`,
                [user.id, status, uid, assignedTimeForDb],
                (errIns) => {
                  if (errIns) {
                    console.error('[DB] INSERT erreur:', errIns);
                    client.publish('pointage/decision', `${uid}|${name}|no`);
                    return;
                  }
                  console.log(`[DB] Arrivée insérée — ${name} (${status})`);
                  client.publish('pointage/decision', `${uid}|${name}|${status}`);
                }
              );
            }
          );
        }
      );
    }
  );
// ✅ Accolade fermante du handler ICI — tout est dedans
});

// ❗ erreurs MQTT
client.on('error', (err) => {
  console.log("Erreur MQTT:", err);
});

// ❗ connexion MySQL
db.connect((err) => {
  if (err) {
    console.log("Erreur connexion MySQL:", err);
  } else {
    console.log("MySQL connecté");
  }
});
const http = require('http');

const httpServer = http.createServer((req, res) => {

  if (req.method === 'POST' && req.url === '/tasks') {

    let body = '';

    req.on('data', chunk => {
      body += chunk;
    });

    req.on('end', () => {

      body = body.trim();

      console.log('[HTTP] tasks reçu:', body);

      client.publish('pointage/tasks', body);

      res.writeHead(200);
      res.end('ok');
    });

  } else {

    res.writeHead(404);
    res.end();
  }
});

httpServer.listen(3001, '0.0.0.0', () => {
  console.log('[HTTP] Serveur tasks sur port 3001');
});