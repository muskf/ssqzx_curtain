const PORT = 3000;
const DB_PATH = './curtain.db';
const STATIC_DIR = 'public';
const SCHEDULE_CHECK_INTERVAL = 60000; // Check and reload schedules every minute
const LOG_RETENTION_DAYS = 7; // Keep logs for 7 days only
const LOG_CLEANUP_INTERVAL = 3600000; // Clean up every hour (in ms)

const express = require('express');
const cors = require('cors');
const bodyParser = require('body-parser');
const schedule = require('node-schedule');
const sqlite3 = require('sqlite3').verbose();
const path = require('path');

const app = express();

app.use(cors());
app.use(bodyParser.json());
app.use(express.static(STATIC_DIR));

// db initialization
const db = new sqlite3.Database(DB_PATH);

db.serialize(() => {
  db.run(`CREATE TABLE IF NOT EXISTS schedules (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    time TEXT NOT NULL,
    command TEXT NOT NULL,
    days TEXT NOT NULL,
    enabled BOOLEAN DEFAULT 1,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
  )`);
  
  db.run(`CREATE TABLE IF NOT EXISTS logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    status TEXT NOT NULL,
    message TEXT NOT NULL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP
  )`);
});

// Current command state
let currentCommand = '';

// Device status storage - default to 'closed'
let deviceStatus = {
  status: 'closed', // Default closed
  lastUpdate: new Date(),
  ip: ''
};

function getActionDesc(command) {
  switch (command) {
    case 'up': return '上升';
    case 'down': return '下降';
    case 'stop': return '停止/解锁';
    case 'lock': return '锁定';
    default: return command;
  }
}

function insertLog(status, message) {
  const timestampStr = new Date().toLocaleString('zh-CN', { timeZone: 'Asia/Shanghai' });
  console.log(`[${timestampStr}] [状态: ${status || '正常'}] ${message}`);
  
  db.run(
    'INSERT INTO logs (status, message) VALUES (?, ?)',
    [status, message],
    function(err) {
      if (err) {
        console.error('日志插入失败:', err);
      }
    }
  );
}

// Cleanup old logs periodically
function cleanupOldLogs() {
  const cutoffDate = new Date();
  cutoffDate.setDate(cutoffDate.getDate() - LOG_RETENTION_DAYS);
  const cutoffStr = cutoffDate.toISOString().split('T')[0] + ' 00:00:00'; // SQLite format
  
  db.run(
    'DELETE FROM logs WHERE timestamp < ?',
    [cutoffStr],
    function(err) {
      if (err) {
        console.error('日志清理失败:', err);
      } else {
        console.log(`已清理 ${this.changes} 条旧日志（保留 ${LOG_RETENTION_DAYS} 天）`);
      }
    }
  );
}

// API Routes

// Get current status
app.get('/api/status', (req, res) => {
  res.json({ command: currentCommand });
  currentCommand = ''; // Clear command after reading
});

app.post('/api/command', (req, res) => {
  const { command } = req.body;
  const currentDeviceStatus = deviceStatus.status;
  
  if (command === 'down' && currentDeviceStatus === 'closed') {
    return res.status(400).json({ success: false, error: '卷帘门已关闭，无法再下降' });
  }
  if (command === 'up' && currentDeviceStatus === 'open') {
    return res.status(400).json({ success: false, error: '卷帘门已打开，无法再上升' });
  }
  if (command === 'lock' && currentDeviceStatus.includes('moving')) {
    currentCommand = 'stop'; // Queue stop first
  }
  
  currentCommand = command;
  const actionDesc = getActionDesc(command);
  const clearMessage = `手动执行 - 卷帘门开始${actionDesc}`;
  insertLog(command, clearMessage);
  
  res.json({ success: true, command });
});

app.post('/api/log', (req, res) => {
  const { status, message } = req.body;
  if (status && ['closed', 'open', 'stopped', 'locked', 'moving_up', 'moving_down'].includes(status)) {
    deviceStatus.status = status;
    deviceStatus.lastUpdate = new Date();
  }
  
  insertLog(status, message);
  
  res.json({ success: true });
});

app.get('/api/schedules', (req, res) => {
  db.all('SELECT * FROM schedules ORDER BY time', (err, rows) => {
    if (err) {
      return res.status(500).json({ error: err.message });
    }
    res.json(rows);
  });
});

app.post('/api/schedules', (req, res) => {
  const { name, time, command, days } = req.body;
  
  db.run(
    'INSERT INTO schedules (name, time, command, days) VALUES (?, ?, ?, ?)',
    [name, time, command, days],
    function(err) {
      if (err) {
        return res.status(500).json({ error: err.message });
      }
      scheduleJobs();
      res.json({ id: this.lastID });
    }
  );
});

app.put('/api/schedules/:id', (req, res) => {
  const { name, time, command, days, enabled } = req.body;
  
  db.run(
    'UPDATE schedules SET name = ?, time = ?, command = ?, days = ?, enabled = ? WHERE id = ?',
    [name, time, command, days, enabled, req.params.id],
    function(err) {
      if (err) {
        return res.status(500).json({ error: err.message });
      }
      scheduleJobs();
      res.json({ changes: this.changes });
    }
  );
});

app.delete('/api/schedules/:id', (req, res) => {
  db.run('DELETE FROM schedules WHERE id = ?', req.params.id, function(err) {
    if (err) {
      return res.status(500).json({ error: err.message });
    }
    scheduleJobs();
    res.json({ deleted: this.changes });
  });
});

app.get('/api/logs', (req, res) => {
  const limit = req.query.limit || 50;
  db.all(
    'SELECT * FROM logs ORDER BY timestamp DESC LIMIT ?',
    [limit],
    (err, rows) => {
      if (err) {
        return res.status(500).json({ error: err.message });
      }
      res.json(rows);
    }
  );
});

app.get('/api/device-status', (req, res) => {
  res.json(deviceStatus);
});

app.post('/api/device-status', (req, res) => {
  const { status, ip } = req.body;
  deviceStatus.status = status;
  deviceStatus.lastUpdate = new Date();
  if (ip) deviceStatus.ip = ip;
  
  const heartbeatMsg = `设备心跳 - 来自IP ${ip || 'ESP8266'} - 状态: ${status}`;
  insertLog(status, heartbeatMsg);
  
  res.json({ success: true });
});

function scheduleJobs() {
  Object.keys(schedule.scheduledJobs).forEach(job => {
    schedule.scheduledJobs[job].cancel();
  });
  
  db.all('SELECT * FROM schedules WHERE enabled = 1', (err, rows) => {
    if (err) {
      console.error('加载定时任务失败:', err);
      return;
    }
    
    rows.forEach(task => {
      const [hours, minutes] = task.time.split(':');
      const days = task.days.split(',').map(day => parseInt(day));
      
      const rule = new schedule.RecurrenceRule();
      rule.hour = parseInt(hours);
      rule.minute = parseInt(minutes);
      rule.dayOfWeek = days;
      
      schedule.scheduleJob(rule, () => {
        console.log(`执行定时任务: ${task.name} - ${task.command}`);
        currentCommand = task.command;
        const actionDesc = getActionDesc(task.command);
        const clearExec = `定时任务 "${task.name}" - 卷帘门开始${actionDesc}`;
        insertLog(task.command, clearExec);
      });
    });
    
    console.log(`已安排 ${rows.length} 个定时任务`);
  });
}

setInterval(scheduleJobs, SCHEDULE_CHECK_INTERVAL);
setInterval(cleanupOldLogs, LOG_CLEANUP_INTERVAL);
scheduleJobs();
cleanupOldLogs();

app.get('/', (req, res) => {
  res.sendFile(path.join(__dirname, STATIC_DIR, 'index.html'));
});

app.listen(PORT, () => {
  console.log(`服务器运行在 http://localhost:${PORT}`);
});
