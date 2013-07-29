<?php

/**
 * @file
 * Functionality for Drush command
 * @author Eric Pinxteren <epinxteren@ibuildings.nl>
 * @author Boy Baukema <boy@ibuildings.nl>
 */

/**
 * Basepath for Drupal.org Project Usage
 */
define('DRUPAL_PROJECT_USAGE_URL', 'https://drupal.org/project/usage/');

/**
 * User agent to use for HTTP Requests to Drupal.org
 */
define(
  'DRUPAL_SECTOOLS_USAGE_USER_AGENT',
  'Mozilla/5.0 DrupalSecToolsBot/1.0.0 (https://github.com/ibuildingsnl/drupal_sectools)'
);

/**
 * Register 'modules-usages-status' command.
 *
 * @implements hook_drush_command()
 */
function drupal_sectools_usage_drush_command() {
  $items = array();

  $items['modules-usages-status'] = array(
    'description' => "Create a status report of modules and their usages from Drupal.org as a CSV export.",
    'aliases' => array('mus'),
    'bootstrap' => DRUSH_BOOTSTRAP_DRUSH,
  );

  return $items;
}

/**
 *
 * @return string
 */
function drush_drupal_sectools_usage_status() {
  drush_log('Getting modules', 'info');
  $modules = drupal_sectools_usage_drush_get_modules();
  if ($modules === NULL) {
    return t("Modules list could not load.");
  }

  $rows = array();
  foreach ($modules as $name => $module) {
    $belongs_to_project = isset($module->info['project']);
    $is_part_of_core = (isset($module->info['package']) && $module->info['package'] === 'Core');
    $is_drupal       = ($belongs_to_project && $module->info['project'] === 'drupal');

    if (!$belongs_to_project) {
      drush_log('Skipping: ' . $name . ', does not belong to a project', 'warn');
      continue;
    }

    if ($is_part_of_core || $is_drupal) {
      continue;
    }

    drush_log('Getting info for module: ' . $name, 'info');

    $info = drupal_sectools_usage_get_info_by_name_and_version($module->info['project'], $module->info['version']);

    $row = array(
      'name'                  => $name,
      'enabled'               => (int) $module->status ? 'enabled' : 'installed',
      'php'                   => $module->info['php'],
      'stability'             => drupal_sectools_usage_get_stability_from_version($module->info['version']),
      'version'               => $module->info['version'],
      'usages'                => !isset($info['usages'])                 ? 0 : $info['usages'],
      'latest_version'        => !isset($info['latest_version'])         ? '' : $info['latest_version'],
      'latest_version_usages' => !isset($info['latest_version_usages'])  ? 0 : $info['latest_version_usages'],
    );

    $rows[] = $row;
  }

  drush_log('Generating CSV', 'info');

  return drupal_sectools_usage_to_csv($rows);
}

/**
 * Get a list of modules from drupal with all the current info.
 *
 * Equivalent Drush command: drush pm-list --type=Module --status=enabled
 */
function drupal_sectools_usage_drush_get_modules() {
  // Drupal 7 uses system_rebuild_module_data
  if (function_exists('system_rebuild_module_data')) {
    return system_rebuild_module_data();
  }

  // Drupal 6 uses module_rebuild_cache
  if (function_exists('module_rebuild_cache')) {
    return module_rebuild_cache();
  }

  watchdog(WATCHDOG_ERROR, 'Unable to detect a way to get a list of modules (are you running Drupal <6 or >7 ?');
  return NULL;
}

/**
 * For a given project and version, get the usage information.
 *
 * @param string $project
 *   Project name
 *
 * @param string $version
 *   Installed version
 *
 * @return array
 *   Array with 'latest_version', 'latest_version_usages', 'usages'.
 */
function drupal_sectools_usage_get_info_by_name_and_version($project, $version) {

  $url = DRUPAL_PROJECT_USAGE_URL . $project;
  $file = drupal_sectools_usage_drush_download_file($url, FALSE, 24 * 3600);

  if (!$file) {
    drush_log('Project ' . $project . ' does not appear to be listed on Drupal.org', 'notice');
    return false;
  }

  $html = file_get_contents($file);

  $document = new DOMDocument();
  @$document->loadHTML($html);

  $xpath = new DOMXPath($document);
  $project_version_nodes       = $xpath->query('//table[@id="project-usage-project-releases"]/tbody/tr/td[@class="active"]/a');
  $project_version_usage_nodes = $xpath->query('//table[@id="project-usage-project-releases"]/tbody/tr/td[@class="project-usage-numbers"][1]');

  $info = array();
  $usage = 0;

  foreach ($project_version_nodes as $item_nr => $node) {
    $project_version = trim($node->textContent);

    $info['latest_version'] = $project_version;

    $corresponding_version_usage_node = $project_version_usage_nodes->item($item_nr);
    $usage += (int) trim($corresponding_version_usage_node->textContent);
    $info['latest_version_usages'] = (int) trim($corresponding_version_usage_node->textContent);

    if ($project_version !== $version) {
      continue;
    }

    if (!isset($usages['usages'])) {
      $corresponding_version_usage_node = $project_version_usage_nodes->item($item_nr);
      $usage += (int) trim($corresponding_version_usage_node->textContent);
      $info['usages'] = (int) trim($corresponding_version_usage_node->textContent);
    }
  }

  return $info;
}

/**
 * Copy of drush_download_file that calls our own downloader and
 * forces use of caching.
 *
 * Download a file using wget, curl or file_get_contents, or via download cache.
 *
 * @param string $url
 *   The url of the file to download.
 * @param string $destination
 *   The name of the file to be saved, which may include the full path.
 *   Optional, if omitted the filename will be extracted from the url and the
 *   file downloaded to the current working directory (Drupal root if
 *   bootstrapped).
 * @param integer $cache_duration
 *   The acceptable age of a cached file. If cached file is too old, a fetch
 *   will occur and cache will be updated. Optional, if ommitted the file will
 *   be fetched directly.
 *
 * @return string
 *   The path to the downloaded file, or FALSE if the file could not be
 *   downloaded.
 */
function drupal_sectools_usage_drush_download_file($url, $destination = FALSE, $cache_duration = 0) {
  // Generate destination of omitted.
  if (!$destination) {
    $file = basename(current(explode('?', $url, 2)));
    $destination = getcwd() . '/' . basename($file);
  }

  if ($cache_dir = drush_directory_cache('download')) {
    $cache_name = str_replace(array(':', '/', '?', '='), '-', $url);
    $cache_file = $cache_dir . "/" . $cache_name;
    // Check for cached, unexpired file.
    if (file_exists($cache_file) && filectime($cache_file) > ($_SERVER['REQUEST_TIME'] - $cache_duration)) {
      drush_log(dt('!name retrieved from cache.', array('!name' => $cache_name)));
    }
    else {
      if (_drupal_sectools_usage_drush_download_file($url, $cache_file, TRUE)) {
        // Cache was set just by downloading file to right location.
      }
      elseif (file_exists($cache_file)) {
        drush_log(dt('!name retrieved from an expired cache since refresh failed.', array('!name' => $cache_name)), 'warning');
      }
      else {
        $cache_file = FALSE;
      }
    }

    if ($cache_file && copy($cache_file, $destination)) {
      // Copy cached file to the destination
      return $destination;
    }
  }
  elseif ($return = _drupal_sectools_usage_drush_download_file($url, $destination)) {
    drush_register_file_for_deletion($return);
    return $return;
  }

  // Unable to retrieve from cache nor download.
  return FALSE;
}

/**
 * Download a file using wget, curl or file_get_contents. Does not use download
 * cache.
 *
 * @param string $url
 *   The url of the file to download.
 * @param string $destination
 *   The name of the file to be saved, which may include the full path.
 * @param boolean $overwrite
 *   Overwrite any file thats already at the destination.
 * @return string
 *   The path to the downloaded file, or FALSE if the file could not be
 *   downloaded.
 */
function _drupal_sectools_usage_drush_download_file($url, $destination, $overwrite = TRUE) {
  static $use_wget;
  if ($use_wget === NULL) {
    $use_wget = drush_shell_exec('which wget');
  }

  $destination_tmp = drush_tempnam('download_file');
  if ($use_wget) {
    drush_shell_exec("wget --no-check-certificate -q --timeout=30 --user-agent=" . escapeshellarg(MODULES_USAGES_USER_AGENT) . " -O %s %s", $destination_tmp, $url);
  }
  else {
    drush_shell_exec("curl --fail -s -L --connect-timeout 30 --user-agent " . escapeshellarg(MODULES_USAGES_USER_AGENT) . " -o %s %s", $destination_tmp, $url);
  }
  if (!drush_file_not_empty($destination_tmp) && $file = @file_get_contents($url)) {
    @file_put_contents($destination_tmp, $file);
  }
// HACKED: Also cache 404s
//  if (!drush_file_not_empty($destination_tmp)) {
//    // Download failed.
//    return FALSE;
//  }
//
  drush_move_dir($destination_tmp, $destination, $overwrite);

  // Courtesy cooldown recommended by Drupal Ops
  // so as not to get your IP blacklisted by drupal.org.
  sleep(1);

  return $destination;
}

/**
 * Convert an associative array to CSV content.
 *
 * @param array $rows
 *   Write associative array to CSV text string.
 *
 * @return string
 *   CSV
 */
function drupal_sectools_usage_to_csv($rows) {
  if (count($rows) === 0) {
    return '';
  }

  $output_buffer = fopen("php://output", 'w');

  // Write the header.
  fputcsv($output_buffer, array_keys(reset($rows)));

  // Write the values.
  foreach ($rows as $val) {
    fputcsv($output_buffer, $val);
  }

  fclose($output_buffer);

  return $output_buffer;
}

/**
 * Parse the indicated stability out of the version number.
 *
 * @param string $version
 *   Version number for a module.
 *
 * @return string
 *   Stability (Unstable, Dev, Alpha, Beta, Stable)
 */
function drupal_sectools_usage_get_stability_from_version($version) {
  $results = array();
  if (preg_match('/-(?P<version>.*)/', $version, $results) === 1) {
    $version = $results['version'];
  }

  if (preg_match('/[\d\.]+/', $version) === 1) {
    $status = 'Stable';
  }
  elseif (strpos($version, 'beta') !== FALSE) {
    $status = 'Beta';
  }
  elseif (strpos($version, 'alpha') !== FALSE) {
    $status = 'Alpha';
  }
  elseif (strpos($version, 'dev') !== FALSE) {
    $status = 'Dev';
  }
  elseif (strpos($version, 'unstable') !== FALSE) {
    $status = 'Unstable';
  }
  elseif (strpos($version, 'rc') !== FALSE) {
    $status = 'RC';
  }
  else {
    $status = 'Unknown';
  }
  return $status;
}
