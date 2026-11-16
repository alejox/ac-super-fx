/*
AC Super FX
Copyright (C) 2026 alejoxcode

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#import <AppKit/AppKit.h>

#include <obs-module.h>
#include <plugin-support.h>

/* Actualizador para macOS.
 *
 * Unos segundos despues de abrir OBS consulta la ultima release en GitHub.
 * Si hay una version mas nueva muestra un aviso; solo si el usuario acepta,
 * descarga el .pkg de esa release y lo instala con el instalador de macOS
 * en la carpeta de plugins del usuario. Nunca instala nada sin confirmacion. */

static NSString *const kLatestReleaseURL = @"https://api.github.com/repos/alejox/ac-super-fx/releases/latest";
static NSString *const kDownloadPrefix = @"https://github.com/alejox/ac-super-fx/releases/download/";
static NSString *const kReleasesPage = @"https://github.com/alejox/ac-super-fx/releases/latest";
static NSString *const kAssetSuffix = @"-macos-universal.pkg";
static NSString *const kSkippedVersionKey = @"ACSuperFXSkippedVersion";
static NSString *const kDisabledKey = @"ACSuperFXDisableUpdateCheck";

static const int64_t kStartDelaySeconds = 10;

static NSURLSession *updater_session;
static dispatch_block_t updater_pending;
static BOOL updater_stopped;

static NSString *updater_text(const char *key)
{
    NSString *value = [NSString stringWithUTF8String:obs_module_text(key)];
    return value ?: @"";
}

static NSString *updater_text_with_version(const char *key, NSString *version)
{
    return [updater_text(key) stringByReplacingOccurrencesOfString:@"%1" withString:version];
}

static void updater_show_result(NSString *title, NSString *info, BOOL offer_page)
{
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = title;
    alert.informativeText = info;
    [alert addButtonWithTitle:updater_text("ACFX.Update.OK")];
    if (offer_page)
        [alert addButtonWithTitle:updater_text("ACFX.Update.OpenPage")];

    NSModalResponse response = [alert runModal];
    if (offer_page && response == NSAlertSecondButtonReturn) {
        NSURL *page = [NSURL URLWithString:kReleasesPage];
        if (page)
            [[NSWorkspace sharedWorkspace] openURL:page];
    }
}

static void updater_report_failure(NSString *version)
{
    dispatch_async(dispatch_get_main_queue(), ^{
        if (updater_stopped)
            return;
        updater_show_result(updater_text_with_version("ACFX.Update.Failed.Title", version),
                            updater_text("ACFX.Update.Failed.Info"), YES);
    });
}

static void updater_install_package(NSURL *package, NSString *version)
{
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/sbin/installer"];
    task.arguments = @[@"-pkg", package.path, @"-target", @"CurrentUserHomeDirectory"];
    task.standardOutput = [NSFileHandle fileHandleWithNullDevice];
    task.standardError = [NSFileHandle fileHandleWithNullDevice];
    task.terminationHandler = ^(NSTask *finished) {
        int status = finished.terminationStatus;
        [[NSFileManager defaultManager] removeItemAtURL:package error:nil];

        if (status != 0) {
            obs_log(LOG_WARNING, "update to %s failed: installer exited with %d", version.UTF8String, status);
            updater_report_failure(version);
            return;
        }

        obs_log(LOG_INFO, "updated to %s; restart OBS to load it", version.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            if (updater_stopped)
                return;
            updater_show_result(updater_text_with_version("ACFX.Update.Done.Title", version),
                                updater_text("ACFX.Update.Done.Info"), NO);
        });
    };

    NSError *error = nil;
    if (![task launchAndReturnError:&error]) {
        obs_log(LOG_WARNING, "update to %s failed: could not start the installer", version.UTF8String);
        updater_report_failure(version);
    }
}

static void updater_download(NSURL *asset, NSString *version)
{
    NSURLSessionDownloadTask *download = [updater_session
        downloadTaskWithURL:asset completionHandler:^(NSURL *location, NSURLResponse *response, NSError *error) {
            NSInteger code = 0;
            if ([response isKindOfClass:[NSHTTPURLResponse class]])
                code = ((NSHTTPURLResponse *) response).statusCode;

            if (error || !location || code != 200) {
                obs_log(LOG_WARNING, "update to %s failed: download error", version.UTF8String);
                updater_report_failure(version);
                return;
            }

            NSString *name = [NSString stringWithFormat:@"ac-super-fx-%@.pkg", version];
            NSURL *package = [[NSURL fileURLWithPath:NSTemporaryDirectory()] URLByAppendingPathComponent:name];
            NSFileManager *files = [NSFileManager defaultManager];
            [files removeItemAtURL:package error:nil];

            NSError *move_error = nil;
            if (![files moveItemAtURL:location toURL:package error:&move_error]) {
                updater_report_failure(version);
                return;
            }

            updater_install_package(package, version);
        }];
    [download resume];
}

static void updater_offer(NSString *version, NSURL *asset)
{
    if (updater_stopped)
        return;

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = updater_text_with_version("ACFX.Update.Title", version);
    alert.informativeText =
        updater_text_with_version("ACFX.Update.Info", [NSString stringWithUTF8String:PLUGIN_VERSION] ?: @"");
    [alert addButtonWithTitle:updater_text("ACFX.Update.Now")];
    [alert addButtonWithTitle:updater_text("ACFX.Update.Later")];
    [alert addButtonWithTitle:updater_text("ACFX.Update.Skip")];

    NSModalResponse response = [alert runModal];
    if (response == NSAlertFirstButtonReturn) {
        updater_download(asset, version);
    } else if (response == NSAlertThirdButtonReturn) {
        [[NSUserDefaults standardUserDefaults] setObject:version forKey:kSkippedVersionKey];
    }
}

static void updater_handle_release(NSData *data)
{
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![parsed isKindOfClass:[NSDictionary class]])
        return;

    NSDictionary *release = parsed;
    id tag = release[@"tag_name"];
    id assets = release[@"assets"];
    if (![tag isKindOfClass:[NSString class]] || ![assets isKindOfClass:[NSArray class]])
        return;

    NSString *version = tag;
    NSString *current = [NSString stringWithUTF8String:PLUGIN_VERSION] ?: @"";
    if ([version compare:current options:NSNumericSearch] != NSOrderedDescending)
        return;

    NSString *skipped = [[NSUserDefaults standardUserDefaults] stringForKey:kSkippedVersionKey];
    if (skipped && [skipped isEqualToString:version])
        return;

    /* Solo se acepta el .pkg de macOS publicado en las releases de este repositorio. */
    NSURL *asset = nil;
    for (id entry in (NSArray *) assets) {
        if (![entry isKindOfClass:[NSDictionary class]])
            continue;
        id name = ((NSDictionary *) entry)[@"name"];
        id url = ((NSDictionary *) entry)[@"browser_download_url"];
        if (![name isKindOfClass:[NSString class]] || ![url isKindOfClass:[NSString class]])
            continue;
        if ([(NSString *) name hasSuffix:kAssetSuffix] && [(NSString *) url hasPrefix:kDownloadPrefix]) {
            asset = [NSURL URLWithString:url];
            break;
        }
    }
    if (!asset)
        return;

    obs_log(LOG_INFO, "update available: %s (installed: %s)", version.UTF8String, PLUGIN_VERSION);
    dispatch_async(dispatch_get_main_queue(), ^{
        updater_offer(version, asset);
    });
}

static void updater_check(void)
{
    if (updater_stopped || [[NSUserDefaults standardUserDefaults] boolForKey:kDisabledKey])
        return;

    NSURL *url = [NSURL URLWithString:kLatestReleaseURL];
    if (!url)
        return;

    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
    request.timeoutInterval = 15.0;
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    [request setValue:[NSString stringWithFormat:@"ac-super-fx/%s", PLUGIN_VERSION] forHTTPHeaderField:@"User-Agent"];

    updater_session = [NSURLSession sessionWithConfiguration:[NSURLSessionConfiguration ephemeralSessionConfiguration]];
    NSURLSessionDataTask *task = [updater_session
        dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            NSInteger code = 0;
            if ([response isKindOfClass:[NSHTTPURLResponse class]])
                code = ((NSHTTPURLResponse *) response).statusCode;
            if (error || !data || code != 200 || updater_stopped)
                return;
            updater_handle_release(data);
        }];
    [task resume];
}

void ac_super_fx_updater_start(void)
{
    updater_stopped = NO;
    updater_pending = dispatch_block_create(0, ^{
        updater_check();
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, kStartDelaySeconds * (int64_t) NSEC_PER_SEC),
                   dispatch_get_main_queue(), updater_pending);
}

void ac_super_fx_updater_stop(void)
{
    updater_stopped = YES;
    if (updater_pending) {
        dispatch_block_cancel(updater_pending);
        updater_pending = nil;
    }
    [updater_session invalidateAndCancel];
    updater_session = nil;
}
