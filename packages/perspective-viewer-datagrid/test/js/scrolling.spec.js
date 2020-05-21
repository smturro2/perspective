/******************************************************************************
 *
 * Copyright (c) 2017, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */

const utils = require("@finos/perspective-test");
const path = require("path");

utils.with_server({}, () => {
    describe.page(
        "superstore.html",
        () => {
            test.capture("scrolls vertically", async page => {
                const datagrid = await page.$("perspective-datagrid");
                await page.evaluate(element => {
                    element.scrollTop = 0;
                    element.scrollLeft = 0;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
                await page.evaluate(element => {
                    element.scrollTop = 300;
                }, datagrid);
            });

            test.capture("scrolls horizontally", async page => {
                const datagrid = await page.$("perspective-datagrid");
                await page.evaluate(element => {
                    element.scrollTop = 0;
                    element.scrollLeft = 0;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
                await page.evaluate(element => {
                    element.scrollLeft = 300;
                }, datagrid);
            });

            test.capture("scrolls both", async page => {
                const datagrid = await page.$("perspective-datagrid");
                await page.evaluate(element => {
                    element.scrollTop = 0;
                    element.scrollLeft = 0;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
                await page.evaluate(element => {
                    element.scrollTop = 300;
                    element.scrollLeft = 300;
                }, datagrid);
            });

            test.capture("scroll past horizontal max", async page => {
                const datagrid = await page.$("perspective-datagrid");
                await page.evaluate(element => {
                    element.scrollTop = 0;
                    element.scrollLeft = 0;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
                await page.evaluate(element => {
                    element.scrollLeft = 300000;
                }, datagrid);
            });

            test.capture("scroll past vertical max", async page => {
                const datagrid = await page.$("perspective-datagrid");
                await page.evaluate(element => {
                    element.scrollTop = 0;
                    element.scrollLeft = 0;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
                await page.evaluate(element => {
                    element.scrollTop = 300000;
                }, datagrid);
            });

            test.capture("resets scroll position when resized and scrolled to max corner", async page => {
                const datagrid = await page.$("perspective-datagrid");
                await page.evaluate(element => {
                    element.scrollTop = 0;
                    element.scrollLeft = 0;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
                await page.evaluate(element => {
                    element.scrollTop = 300000;
                    element.scrollLeft = 300000;
                }, datagrid);
                await page.shadow_click("perspective-viewer", "#config_button");
            });
        },
        {reload_page: false, root: path.join(__dirname, "..", "..")}
    );
});
