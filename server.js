const mqtt = require('mqtt');
const mysql = require('mysql2');

function normalizeButton(value) {
  if (!value) return null;
  const v = String(value).trim().toLowerCase().replace(/\s+/g, '');
  console.log(`[DEBUG normalizeButton] raw="${value}" normalized="${v}"`);
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

client.on('connect', () => {
  console.log("MQTT connecté");
  client.publish('pointage/decision',      '', { retain: true });
  client.publish('pointage/finish_result', '', { retain: true });
  client.publish('pointage/tasks',         '', { retain: true });
  client.publish('pointage/project',       '', { retain: true });
  client.subscribe('pointage/action');
});

// ✅ publishProjectTasks — sans finish_result
function publishProjectAndTasks(projectId) {
  db.query(
    `SELECT label, completed
FROM tasks
WHERE project_id = 0
ORDER BY sort_order ASC`,
    (errTasks, taskRows) => {
      if (!errTasks) {
        const states = [0, 0, 0, 0];
        taskRows.forEach((task, index) => {
          if (index < 4) states[index] = task.completed ? 1 : 0;
        });
        const payload = states.join('|');
        client.publish('pointage/tasks', payload, { retain: true });
        console.log('[MQTT] tâches republiees:', payload);
        // ✅ fin — pas de finish_result ici
      }
    }
  );
}

client.on('message', (topic, message) => {
  const raw = message.toString().trim();
  const parts = raw.split('|');

  const uid    = normalizeUid(parts[0]);
  const button = normalizeButton(parts[1]);
  const scanId = parts[2] ? parts[2].trim() : '0';

  console.log(`[SCAN] uid=${uid} button=${button}`);

  if (!uid || !button) {
    console.log('[SCAN] Payload invalide');
    client.publish(
  'pointage/decision',
  `${uid || 'UNKNOWN'}|Invalide|no|${scanId}`,
  { retain: false }
);
    return;
  }

  db.query(
    `SELECT id, prenom, nom, button_assigned
     FROM users
     WHERE UPPER(REPLACE(REPLACE(REPLACE(badge_uid, ' ', ''), ':', ''), '-', '')) = ?
       AND active = 1`,
    [uid],
    (err, results) => {
      if (err || results.length === 0) {
        console.log('[DB] Badge inconnu:', uid);
        client.publish(
  'pointage/decision',
  `${uid}|Inconnu|no|${scanId}`,
  { retain: false }
);
        return;
      }

      const user = results[0];
      const name = `${user.prenom} ${user.nom}`;

      const assignedButton = normalizeButton(user.button_assigned);
      if (!assignedButton || assignedButton !== button) {
        console.log(`[SCAN] Mauvais bouton: attendu=${assignedButton} reçu=${button}`);
client.publish(
  'pointage/decision',
  `${uid}|${name}|no|${scanId}`,
  { retain: false }
);        return;
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
            client.publish('pointage/decision', `${uid}|${name}|no|${scanId}`, { retain: false });
            return;
          }

          const lastRow = rows.length > 0 ? rows[0] : null;
          const hasOpenSession = !!lastRow && !lastRow.depart;
          const alreadyClosedToday = !!lastRow && !!lastRow.depart;

          if (alreadyClosedToday) {
            console.log(`[DB] Déjà pointé aujourd'hui — ${name}`);
            db.query(
              // Par :
`SELECT id, code, start_date, due_date,
UNIX_TIMESTAMP(due_date) as due_ts,
UNIX_TIMESTAMP(start_date) as start_ts,
status
FROM projects
WHERE user_id = ?
AND status IN ('progress','pending')
AND due_date IS NOT NULL
ORDER BY CASE WHEN status='progress' THEN 0 ELSE 1 END, created_at ASC LIMIT 1`,
              [user.id],  
              (errProj, projRows) => {
                if (!errProj && projRows.length > 0) {
                  const p = projRows[0];
                  const deadline = p.due_ts * 1000;
                  const estimatedSeconds = Math.abs(p.due_ts - p.start_ts);
                  client.publish('pointage/project', `${p.code}|${estimatedSeconds}|${deadline}|${p.status}`, { retain: true });
                  publishProjectAndTasks(p.id);
                }else {
    // ← AJOUTE
    client.publish('pointage/project', 'Aucun projet|0|0|none', { retain: true });
  }
                const statusToSend = lastRow.status || 'ok';

               client.publish(
                  'pointage/decision',
                  `${uid}|${name}|${statusToSend}|${scanId}`,
                  { retain: false }
);
              }
            );
            return;
          }

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
                  client.publish(
  'pointage/decision',
  `${uid}|${name}|no|${scanId}`,
  { retain: false }
);
                  return;
                }
                console.log(`[DB] Arrivée enregistrée — ${name} (${status})`);
                client.publish('pointage/decision', `${uid}|${name}|${status}|${scanId}`, { retain: false });
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
                  client.publish(
  'pointage/decision',
  `${uid}|${name}|no|${scanId}`,
  { retain: false }
);
                  return;
                }
                const statusToSend = lastRow.status || 'ok';
                console.log(`[DB] Départ enregistré — ${name}`);
                client.publish('pointage/decision', `${uid}|${name}|${statusToSend}|${scanId}`, { retain: false });
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
                    client.publish(
  'pointage/decision',
  `${uid}|${name}|no|${scanId}`,
  { retain: false }
);
                    return;
                  }
                  console.log(`[DB] Arrivée insérée — ${name} (${status})`);
                  client.publish('pointage/decision', `${uid}|${name}|${status}|${scanId}`, { retain: false });
                  db.query(
  `SELECT id, code, start_date, due_date,
   UNIX_TIMESTAMP(due_date) as due_ts,
   UNIX_TIMESTAMP(start_date) as start_ts,
   status
   FROM projects
   WHERE user_id = ?
   AND status IN ('progress','pending')
   AND due_date IS NOT NULL
   ORDER BY CASE WHEN status='progress' THEN 0 ELSE 1 END,
   created_at ASC LIMIT 1`,
  [user.id],
  (e, pr) => {

    if (!e && pr.length > 0) {

      const p = pr[0];

      const deadline = p.due_ts * 1000;

      const estimatedSeconds =
        Math.abs(p.due_ts - p.start_ts);

      client.publish(
        'pointage/project',
        `${p.code}|${estimatedSeconds}|${deadline}|${p.status}`,
        { retain: true }
      );

      publishProjectAndTasks(p.id);

      console.log(
        '[MQTT] Projet envoyé:',
        p.code,
        p.status
      );

    } else {

      client.publish(
        'pointage/project',
        'Aucun projet|0|0|none',
        { retain: true }
      );

      console.log('[MQTT] Aucun projet');
    }
  }
);
                }
                  );
                }
              );
            }
          );
    }
  );
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

  // ✅ finish_result — seul endroit où on publie finish_result
  if (req.method === 'POST' && req.url === '/finish_result') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
    body = body.trim();

     const parts = body.split('|');

    const result = parts[0];
    const userId = parts[1];

    console.log('[HTTP] finish_result reçu:', result, userId);

      client.publish('pointage/finish_result', result, { retain: true });
      client.publish('pointage/tasks', '0|0|0|0', { retain: true });
      db.query(`
      UPDATE tasks
      SET completed = 0,
      completed_at = NULL
      WHERE project_id = 0
`);
db.query(`
    UPDATE projects
    SET checklist_done = 0
    WHERE status IN ('progress','pending')
`);
      client.publish('pointage/project', '', { retain: true });
      res.writeHead(200);
      res.end('ok');
      setTimeout(() => {
        db.query(
          `SELECT id, code, start_date, due_date,
           UNIX_TIMESTAMP(due_date) as due_ts,
           UNIX_TIMESTAMP(start_date) as start_ts,
           status
           FROM projects
           WHERE user_id = ?
          AND status IN ('progress', 'pending') AND due_date IS NOT NULL
           ORDER BY CASE WHEN status='progress' THEN 0 ELSE 1 END, created_at ASC LIMIT 1`,
          [userId], 
          (err, rows) => {
            if (!err && rows.length > 0) {
              const p = rows[0];
              const deadline = p.due_ts * 1000;
              const estimatedSeconds = Math.abs(p.due_ts - p.start_ts);
              client.publish('pointage/project',
                `${p.code}|${estimatedSeconds}|${deadline}|${p.status}`,
                { retain: true });
              publishProjectAndTasks(p.id);
              client.publish(
             'pointage/next_project',
              p.code,
             { retain: true }
    );

console.log(
  '[MQTT] next_project publié:',
  p.code
);
              console.log('[HTTP] Projet suivant publié:', p.code, p.status);
            } else {
      // ← AJOUTE
      client.publish('pointage/project', 'Aucun projet|0|0|none', { retain: true });
    }
          }
        );
      }, 500);

      setTimeout(() => {
        client.publish('pointage/finish_result', '', { retain: true });
        console.log('[HTTP] finish_result retained effacé');
      }, 10000);
    });
    return;
  }

  if (req.method === 'POST' && req.url === '/tasks') {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      body = body.trim();
      console.log('[HTTP] tasks reçu:', body);
      client.publish('pointage/tasks', body, { retain: true });
      // ✅ envoyer prochain projet
      const userId = req.headers['x-user-id'];

db.query(
  `SELECT code
   FROM projects
   WHERE user_id = ?
   AND status = 'pending'
   ORDER BY created_at ASC
   LIMIT 1`,
  [userId],
  (eNext, nextRows) => {

    if (!eNext && nextRows.length > 0) {

      const nextCode = nextRows[0].code;

      client.publish(
        'pointage/next_project',
        nextCode,
        { retain: false }
      );

      console.log('[MQTT] projet suivant:', nextCode);

    } else {

      client.publish(
        'pointage/next_project',
        '',
        { retain: false }
      );

      console.log('[MQTT] aucun projet suivant');
    }
  }
);

      res.writeHead(200);
      res.end('ok');
    });
    return;
  }

  if (req.method === 'POST' && req.url === '/sync') {

  let body = '';

  req.on('data', chunk => {
    body += chunk;
  });

  req.on('end', () => {

    const userId = body.trim();

    console.log('[SYNC] userId reçu:', userId);

    db.query(
      `SELECT id, code, start_date, due_date,
       UNIX_TIMESTAMP(due_date) as due_ts,
       UNIX_TIMESTAMP(start_date) as start_ts,
       status
       FROM projects
       WHERE user_id = ?
       AND status IN ('progress','pending')
       AND due_date IS NOT NULL
       ORDER BY CASE WHEN status='progress' THEN 0 ELSE 1 END, created_at ASC LIMIT 1`,
      [userId],
      (err, rows) => {

        if (!err && rows.length > 0) {

          const p = rows[0];

          const deadline = p.due_ts * 1000;
          const estimatedSeconds =
            Math.abs(p.due_ts - p.start_ts);

          client.publish(
            'pointage/project',
            `${p.code}|${estimatedSeconds}|${deadline}|${p.status}`,
            { retain: true }
          );

          publishProjectAndTasks(p.id);

          console.log('[SYNC] Projet publié:', p.code, p.status);

        } else {

          client.publish(
            'pointage/project',
            'Aucun projet|0|0|none',
            { retain: true }
          );

          console.log('[SYNC] Aucun projet pour user:', userId);
        }

        res.writeHead(200);
        res.end('ok');
      }
    );
  });

  return;
}

  res.writeHead(404);
  res.end();
});

httpServer.listen(3001, '0.0.0.0', () => {
  console.log('[HTTP] Serveur tasks sur port 3001');
});