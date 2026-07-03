import fs from 'fs';
import path from 'path';
import { execSync } from 'node:child_process';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// ANSI color codes for better output
const colors = {
  reset: '\x1b[0m',
  bright: '\x1b[1m',
  green: '\x1b[32m',
  red: '\x1b[31m',
  yellow: '\x1b[33m',
};

function log(message, color = 'reset') {
  console.log(`${colors[color]}${message}${colors.reset}`);
}

function logSection(message) {
  log('\n' + '='.repeat(32), 'bright');
  log(`   ${message}`, 'bright');
  log('='.repeat(32) + '\n', 'bright');
}

async function fileExists(filePath) {
  try {
    await fs.promises.access(filePath, fs.constants.F_OK);
    return true;
  } catch {
    return false;
  }
}

/** Resolve the test project's own EngineAssociation first: a GUID maps to a
 *  registered source build via the registry, a version string to a launcher
 *  install. Falls back to newest launcher install found. */
function findAssociatedEngineRoot() {
  try {
    const { projectFile } = getProjectPaths();
    const uproject = JSON.parse(fs.readFileSync(projectFile, 'utf-8'));
    const association = String(uproject.EngineAssociation ?? '').replace(/^\{|\}$/g, '');
    if (!association) return null;

    if (/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(association)) {
      const output = execSync(
        `reg query "HKCU\\SOFTWARE\\Epic Games\\Unreal Engine\\Builds" /v "{${association}}"`,
        { stdio: 'pipe', encoding: 'utf-8' },
      );
      const match = output.match(/REG_SZ\s+(.+)/);
      if (match && fs.existsSync(match[1].trim())) {
        return match[1].trim();
      }
      return null;
    }

    for (const basePath of ENGINE_SEARCH_ROOTS) {
      const root = path.join(basePath, `UE_${association}`);
      if (fs.existsSync(root)) return root;
    }
  } catch {
    // Malformed uproject / registry miss - fall through to the generic search.
  }
  return null;
}

const ENGINE_SEARCH_ROOTS = [
  'C:/Program Files/Epic Games',
  'D:/Program Files/Epic Games',
  'E:/Program Files/Epic Games',
  'C:/Epic Games',
  'D:/Epic Games',
  'E:/Epic Games',
];

function findUEBuildTool() {
  // Check for environment variable override first
  const envPath = process.env.UE_BUILD_TOOL_PATH;
  if (envPath) {
    return envPath;
  }

  // The engine the test project is associated with wins.
  const associatedRoot = findAssociatedEngineRoot();
  if (associatedRoot) {
    const associatedBuildTool = path.join(associatedRoot, 'Engine', 'Build', 'BatchFiles', 'Build.bat');
    if (fs.existsSync(associatedBuildTool)) {
      return associatedBuildTool;
    }
  }

  // Same search roots as src/editor-control.ts so `npm run build` and the
  // server's editor control resolve the same engine install.
  const versions = ['5.8', '5.7', '5.6', '5.5', '5.4', '5.3'];
  for (const basePath of ENGINE_SEARCH_ROOTS) {
    for (const version of versions) {
      const buildToolPath = path.join(basePath, `UE_${version}`, 'Engine', 'Build', 'BatchFiles', 'Build.bat');
      if (fs.existsSync(buildToolPath)) {
        return buildToolPath;
      }
    }
  }

  return null;
}

function getProjectPaths() {
  const projectRoot = path.resolve(__dirname, '..', 'tests', 'ue_mcp');
  const projectFile = path.join(projectRoot, 'ue_mcp.uproject');
  return { projectRoot, projectFile };
}

export {
  colors,
  log,
  logSection,
  fileExists,
  findUEBuildTool,
  getProjectPaths,
};
