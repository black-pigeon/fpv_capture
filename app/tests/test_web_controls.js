'use strict';
// Exercise real UI state and submit handlers without a browser dependency.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
function element(id = '') {
    return {id, value:'', checked:false, disabled:false, hidden:false, firstChild:{textContent:''},
        handlers:{}, addEventListener(type, fn) {this.handlers[type]=fn;},
        replaceChildren() {}, append() {}, setAttribute() {}, reportValidity() {return true;},
        insertRow() {return element();}, insertCell() {return element();}};
}
const nodes = new Map();
function get(id) {if(!nodes.has(id))nodes.set(id,element(id));return nodes.get(id);}
get('scan-form').elements = ['bands','step','repeat','dwell','settle'].map(id => Object.assign(get(id),{tagName:'INPUT'}));
const state = {simulate:false,running:true,mode:'scan',phase:'observing',switching:false,stopping:false,
    frequency_locked:false,signal_present:false,tracks:[],channels:[],gain_target_db:50,gain_pending:false,
    config:{mode:'scan',bands:[[5650e6,5950e6]],step_mhz:6,gain_db:50,dwell_ms:150,settle_ms:20,repeat:1}};
let current=state;const posts=[];
const context = vm.createContext({document:{getElementById:get,createElement:()=>element()},
    URLSearchParams, Date, setTimeout() {},
    fetch:async (url, options={}) => {
        if (options.method==='POST') posts.push({url,body:options.body});
        return {ok:true,json:async()=>current};
    }});
(async()=>{
    vm.runInContext(fs.readFileSync(path.join(__dirname,'../web/app.js'),'utf8'),context);
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(get('lock-frequency').disabled,false,'frequency entry must stay editable during scanning');
    assert.equal(get('lock').disabled,false,'lock must be available during scanning');
    assert.equal(get('bands').disabled,true,'scan ranges still require stopping');
    get('lock-frequency').value='5805';
    get('lock-form').handlers.submit({preventDefault(){}});
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(posts.length,1);
    assert.equal(posts[0].url,'/api/start','direct transition must not issue a stop command');
    const form=new URLSearchParams(posts[0].body);
    assert.equal(form.get('mode'),'lock');assert.equal(form.get('frequency_mhz'),'5805');
    assert.equal(form.get('gain_db'),'50');assert.equal(form.get('step_mhz'),'6');
    current={...state,switching:true};context.testState=current;
    vm.runInContext('render(testState, true)',context);
    assert.equal(get('lock').disabled,true);assert.equal(get('lock-frequency').disabled,true);
    current={...state,mode:'lock',frequency_locked:true,lock_hz:5805e6};context.testState=current;
    vm.runInContext('render(testState, true)',context);
    assert.equal(get('lock').disabled,false,'a running lock also permits changing frequency');
    assert.equal(get('unlock').disabled,false);
    get('unlock').handlers.click();
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(new URLSearchParams(posts[1].body).get('mode'),'scan');
    console.log('PASS live lock controls: scan to lock, lock retune, unlock, busy state, preserved settings');
})().catch(e=>{console.error(e);process.exitCode=1;});
